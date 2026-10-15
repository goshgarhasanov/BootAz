#include "Fat32.h"
#include "I18n.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <ctime>

namespace bootaz {

namespace {

inline void put16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); }
inline void put32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (8 * i)); }
inline uint16_t get16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
inline uint32_t get32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24); }

constexpr uint32_t kEoc = 0x0FFFFFFF;
constexpr uint32_t kMinClusters = 65525;

bool shortChar(unsigned char c)
{
    return std::isupper(c) || std::isdigit(c) || std::strchr("!#$%&'()-@^_`{}~", c) != nullptr;
}

uint8_t lfnChecksum(const uint8_t* sn)
{
    uint8_t sum = 0;
    for (int i = 0; i < 11; ++i)
        sum = uint8_t(((sum & 1) << 7) + (sum >> 1) + sn[i]);
    return sum;
}

} // namespace

uint32_t fat32DefaultCluster(uint64_t bytes)
{
    // Same steps as Rufus / Microsoft's defaults, extended past 32 GB.
    if (bytes < 64 * MiB) return 512;
    if (bytes < 128 * MiB) return 1024;
    if (bytes < 256 * MiB) return 2048;
    if (bytes < 8 * GiB) return 4096;
    if (bytes < 16 * GiB) return 8192;
    if (bytes < 32 * GiB) return 16384;
    return 32768;
}

std::string fat32Label(const std::string& label)
{
    std::string s;
    for (unsigned char c : label) {
        if (s.size() == 11)
            break;
        c = static_cast<unsigned char>(std::toupper(c));
        if (c < 0x20 || c >= 0x7f || std::strchr("\"*+,./:;<=>?[\\]|", c))
            c = '_';
        s += char(c);
    }
    return s.empty() ? "NO NAME" : s;
}

Fat32Writer::Fat32Writer(BlockTarget& target, const Fat32Params& p, const JobContext& ctx) : t_(target), ctx_(ctx)
{
    bps_ = t_.sectorSize();
    const uint64_t totalSectors64 = t_.size() / bps_;
    if (totalSectors64 > 0xFFFFFFFFULL)
        throw Error(T("The partition is too large for FAT32"));
    const uint32_t totalSectors = uint32_t(totalSectors64);
    clusterBytes_ = p.clusterSize ? p.clusterSize : fat32DefaultCluster(t_.size());
    clusterBytes_ = std::max(clusterBytes_, bps_);

    // Pick the geometry; shrink the cluster size until FAT32's minimum cluster count is met.
    for (;;) {
        spc_ = clusterBytes_ / bps_;
        if (spc_ == 0 || spc_ > 128 || (spc_ & (spc_ - 1)))
            throw Error(T("Invalid cluster size for FAT32"));
        const uint64_t tmp1 = uint64_t(totalSectors) - reserved_;
        const uint64_t tmp2 = (256ULL * spc_ + 2) / 2;
        fatSectors_ = uint32_t((tmp1 + tmp2 - 1) / tmp2);
        clusters_ = (totalSectors - reserved_ - 2 * fatSectors_) / spc_;
        if (clusters_ >= kMinClusters || spc_ == 1)
            break;
        clusterBytes_ /= 2;
    }
    if (clusters_ < kMinClusters)
        throw Error(T("The partition is too small for FAT32 (at least 33 MB is needed)"));
    if (clusters_ > 0x0FFFFFF5)
        throw Error(T("Too many clusters for FAT32; choose a larger cluster size"));
    dataStart_ = uint64_t(reserved_ + 2 * fatSectors_) * bps_;
    label_ = fat32Label(p.label);

    const std::time_t now = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    date_ = uint16_t(((tm.tm_year - 80) << 9) | ((tm.tm_mon + 1) << 5) | tm.tm_mday);
    time_ = uint16_t((tm.tm_hour << 11) | (tm.tm_min << 5) | (tm.tm_sec / 2));

    ctx.log(format(T("Formatting FAT32: %1 clusters of %2"), {std::to_string(clusters_), humanBytes(clusterBytes_)}));

    // Boot sector + FSInfo (and their backups at sector 6).
    std::vector<uint8_t> bs(bps_, 0);
    const uint8_t jmp[3] = {0xEB, 0x58, 0x90};
    std::memcpy(bs.data(), jmp, 3);
    std::memcpy(bs.data() + 3, "MSWIN4.1", 8);
    put16(bs.data() + 11, uint16_t(bps_));
    bs[13] = uint8_t(spc_);
    put16(bs.data() + 14, uint16_t(reserved_));
    bs[16] = 2;
    bs[21] = 0xF8;
    put16(bs.data() + 24, 63);
    put16(bs.data() + 26, 255);
    put32(bs.data() + 28, p.hiddenSectors);
    put32(bs.data() + 32, totalSectors);
    put32(bs.data() + 36, fatSectors_);
    put32(bs.data() + 44, 2);   // root cluster
    put16(bs.data() + 48, 1);   // FSInfo
    put16(bs.data() + 50, 6);   // backup boot sector
    bs[64] = 0x80;
    bs[66] = 0x29;
    put32(bs.data() + 67, p.volumeId ? p.volumeId : uint32_t((uint32_t(now) * 2654435761u) ^ (uint32_t(date_) << 16 | time_)));
    std::memset(bs.data() + 71, ' ', 11);
    std::memcpy(bs.data() + 71, label_.data(), label_.size());
    std::memcpy(bs.data() + 82, "FAT32   ", 8);
    // Boot code: print a short message when someone tries to boot a non-bootable drive.
    {
        static const uint8_t code[] = {0xFA, 0x31, 0xC0, 0x8E, 0xD8, 0x8E, 0xD0, 0xBC, 0x00, 0x7C, 0xFB, 0xBE, 0x7C, 0x7C,
                                       0xAC, 0x84, 0xC0, 0x74, 0x09, 0xB4, 0x0E, 0xBB, 0x07, 0x00, 0xCD, 0x10, 0xEB, 0xF2,
                                       0x31, 0xC0, 0xCD, 0x16, 0xCD, 0x19};
        std::memcpy(bs.data() + 90, code, sizeof code);
        const char msg[] = "\r\nThis drive is not bootable (BootAz).\r\nRemove it and press any key...\r\n";
        std::memcpy(bs.data() + 90 + sizeof code, msg, sizeof msg);   // the code loads it from 0x7C7C
    }
    bs[bps_ - 2] = 0x55;
    bs[bps_ - 1] = 0xAA;
    if (bps_ > 512) {   // the 0x55AA signature also belongs at offset 510
        bs[510] = 0x55;
        bs[511] = 0xAA;
    }

    std::vector<uint8_t> fsi(bps_, 0);
    put32(fsi.data(), 0x41615252);
    put32(fsi.data() + 484, 0x61417272);
    put32(fsi.data() + 488, clusters_ - 1);
    put32(fsi.data() + 492, 3);
    put32(fsi.data() + 508, 0xAA550000);

    // Zero the reserved area, both FATs and the root cluster in large writes.
    const uint64_t zeroEnd = dataStart_ + clusterBytes_;
    std::vector<uint8_t> zero(4 * MiB, 0);
    for (uint64_t off = 0; off < zeroEnd;) {
        ctx.checkCancel();
        const size_t n = size_t(std::min<uint64_t>(zero.size(), zeroEnd - off));
        t_.writeAt(off, zero.data(), n);
        off += n;
        ctx.progress(T("Formatting"), off, zeroEnd);
    }
    t_.writeAt(0, bs.data(), bps_);
    t_.writeAt(uint64_t(bps_), fsi.data(), bps_);
    t_.writeAt(6ULL * bps_, bs.data(), bps_);
    t_.writeAt(7ULL * bps_, fsi.data(), bps_);
    // FSInfo's third sector is all zero except the trailing signature in some formatters; harmless.

    fat_ = {0x0FFFFFF8, kEoc, kEoc};
    DirNode root;
    root.chain = {2};
    dirs_.push_back(root);
    // Volume label entry in the root folder.
    Rec lab;
    std::memset(lab.shortName, ' ', 11);
    std::memcpy(lab.shortName, label_.data(), label_.size());
    lab.attr = 0x08;
    lab.longName = "\x01label";   // never collides with a real name
    dirs_[0].recs.push_back(lab);
}

uint64_t Fat32Writer::clusterOffset(uint32_t c) const { return dataStart_ + uint64_t(c - 2) * clusterBytes_; }

uint64_t Fat32Writer::freeBytes() const { return uint64_t(clusters_ + 2 - next_) * clusterBytes_; }

uint32_t Fat32Writer::allocate(uint32_t count)
{
    if (uint64_t(next_) + count > uint64_t(clusters_) + 2)
        throw Error(T("Not enough space on the drive for these files"));
    const uint32_t first = next_;
    next_ += count;
    return first;
}

void Fat32Writer::setChain(const std::vector<uint32_t>& c)
{
    if (c.empty())
        return;
    const uint32_t maxC = *std::max_element(c.begin(), c.end());
    if (fat_.size() <= maxC)
        fat_.resize(size_t(maxC) + 1, 0);
    for (size_t i = 0; i < c.size(); ++i)
        fat_[c[i]] = i + 1 < c.size() ? c[i + 1] : kEoc;
}

int Fat32Writer::dirFor(const std::string& path, bool create)
{
    int cur = 0;
    for (const std::string& part : split(path, '/')) {
        if (part.empty())
            continue;
        const std::string key = lower(part);
        auto it = dirs_[size_t(cur)].children.find(key);
        if (it != dirs_[size_t(cur)].children.end()) {
            cur = it->second;
            continue;
        }
        if (!create)
            return -1;
        DirNode d;
        d.parent = cur;
        dirs_.push_back(d);
        const int idx = int(dirs_.size() - 1);
        dirs_[size_t(cur)].children[key] = idx;
        addRec(cur, part, 0x10, idx, 0, 0);
        cur = idx;
    }
    return cur;
}

void Fat32Writer::addRec(int dirIdx, const std::string& name, uint8_t attr, int childDir, uint32_t cluster, uint32_t size)
{
    DirNode& d = dirs_[size_t(dirIdx)];
    const std::string key = lower(name);
    if (d.longNames.count(key))
        throw Error(format(T("Duplicate file name: %1"), {name}));
    d.longNames[key] = true;
    if (utf8ToUtf16(name).size() > 255)
        throw Error(format(T("File name too long for FAT32: %1"), {name}));

    Rec r;
    r.longName = name;
    r.attr = attr;
    r.dir = childDir;
    r.cluster = cluster;
    r.size = size;

    // Is the name already a valid 8.3 name, differing at most in all-lower-case parts?
    const size_t dot = name.rfind('.');
    std::string base = dot == std::string::npos ? name : name.substr(0, dot);
    std::string ext = dot == std::string::npos ? "" : name.substr(dot + 1);
    auto valid83 = [&](const std::string& part, size_t maxLen, bool& allLower) {
        if (part.empty() || part.size() > maxLen)
            return false;
        bool lowerSeen = false, upperSeen = false;
        for (unsigned char c : part) {
            if (std::islower(c)) lowerSeen = true;
            if (std::isupper(c)) upperSeen = true;
            if (!shortChar(static_cast<unsigned char>(std::toupper(c))))
                return false;
        }
        if (lowerSeen && upperSeen)
            return false;
        allLower = lowerSeen;
        return true;
    };
    bool baseLower = false, extLower = false;
    const bool fits = name != "." && name != ".." && valid83(base, 8, baseLower) &&
                      (ext.empty() || valid83(ext, 3, extLower)) && (dot == std::string::npos || !ext.empty());
    std::memset(r.shortName, ' ', 11);
    if (fits) {
        const std::string B = upper(base), E = upper(ext);
        std::memcpy(r.shortName, B.data(), B.size());
        std::memcpy(r.shortName + 8, E.data(), E.size());
        r.caseFlags = uint8_t((baseLower ? 0x08 : 0) | (extLower ? 0x10 : 0));
        r.needLfn = false;
    } else {
        // Generate BASE~N.EXT.
        auto clean = [](const std::string& s, size_t maxLen) {
            std::string o;
            for (unsigned char c : s) {
                if (c == ' ' || c == '.')
                    continue;
                const unsigned char u = static_cast<unsigned char>(std::toupper(c));
                o += (c < 0x80 && shortChar(u)) ? char(u) : '_';
                if (o.size() == maxLen)
                    break;
            }
            return o;
        };
        std::string B = clean(base, 8), E = clean(ext, 3);
        if (B.empty())
            B = "_";
        for (int n = 1;; ++n) {
            const std::string tail = "~" + std::to_string(n);
            std::string cand = B.substr(0, std::min(B.size(), 8 - tail.size())) + tail;
            std::string sn = cand;
            sn.resize(8, ' ');
            std::string full = sn + (E + "   ").substr(0, 3);
            if (!d.shortNames.count(full)) {
                std::memcpy(r.shortName, full.data(), 11);
                break;
            }
            if (n > 999999)
                throw Error("Cannot make a unique short name");
        }
        r.needLfn = true;
    }
    d.shortNames[std::string(reinterpret_cast<char*>(r.shortName), 11)] = true;
    d.recs.push_back(r);
}

void Fat32Writer::makeDir(const std::string& path) { dirFor(path, true); }

void Fat32Writer::writeFile(const std::string& path, uint64_t size, const FileSource& source)
{
    if (finished_)
        throw Error("Writer already finished");
    if (size >= 4 * GiB)
        throw Error(format(T("%1 is larger than 4 GB, which FAT32 cannot store"), {path}));
    const size_t slash = path.rfind('/');
    const std::string dirPath = slash == std::string::npos ? "" : path.substr(0, slash);
    const std::string name = path.substr(slash == std::string::npos ? 0 : slash + 1);
    const int dir = dirFor(dirPath, true);

    const uint32_t count = uint32_t((size + clusterBytes_ - 1) / clusterBytes_);
    uint32_t first = 0;
    if (count) {
        first = allocate(count);
        std::vector<uint32_t> chain(count);
        for (uint32_t i = 0; i < count; ++i)
            chain[i] = first + i;
        setChain(chain);
    }
    const uint64_t base = count ? clusterOffset(first) : 0;
    uint64_t written = 0;
    std::vector<uint8_t> carry;   // keeps writes sector aligned
    source([&](const uint8_t* p, size_t n) {
        if (written + carry.size() + n > size)
            throw Error(format(T("%1 is bigger than expected"), {path}));
        if (!carry.empty() || (n % bps_) != 0) {
            carry.insert(carry.end(), p, p + n);
            const size_t whole = carry.size() / bps_ * bps_;
            if (whole) {
                t_.writeAt(base + written, carry.data(), whole);
                written += whole;
                carry.erase(carry.begin(), carry.begin() + long(whole));
            }
        } else {
            t_.writeAt(base + written, p, n);
            written += n;
        }
    });
    if (!carry.empty()) {
        carry.resize((carry.size() + bps_ - 1) / bps_ * bps_, 0);
        t_.writeAt(base + written, carry.data(), carry.size());
        written += carry.size();
    }
    if (written < size)
        throw Error(format(T("%1 is shorter than expected"), {path}));
    addRec(dir, name, 0x20, -1, first, uint32_t(size));
    files_[lower(path)] = {first, size};
}

void Fat32Writer::readBack(const std::string& path, uint64_t size, const FileSink& sink)
{
    const auto it = files_.find(lower(path));
    if (it == files_.end())
        throw Error(format(T("%1 is missing on the drive"), {path}));
    if (it->second.second != size)
        throw Error(format(T("%1 has the wrong size on the drive"), {path}));
    if (size == 0)
        return;
    const uint64_t base = clusterOffset(it->second.first);   // files are stored contiguously
    std::vector<uint8_t> buf(4 * MiB);
    for (uint64_t done = 0; done < size;) {
        const size_t n = size_t(std::min<uint64_t>(buf.size(), size - done));
        const size_t aligned = (n + bps_ - 1) / bps_ * bps_;
        if (aligned > buf.size())
            buf.resize(aligned);
        t_.readAt(base + done, buf.data(), aligned);
        sink(buf.data(), n);
        done += n;
    }
}

void Fat32Writer::serializeDir(int idx)
{
    DirNode& d = dirs_[size_t(idx)];
    std::vector<uint8_t> raw;
    auto entry = [&](const uint8_t* sn, uint8_t attr, uint8_t caseFlags, uint32_t cluster, uint32_t size) {
        uint8_t e[32] = {};
        std::memcpy(e, sn, 11);
        e[11] = attr;
        e[12] = caseFlags;
        put16(e + 14, time_);
        put16(e + 16, date_);
        put16(e + 18, date_);
        put16(e + 20, uint16_t(cluster >> 16));
        put16(e + 22, time_);
        put16(e + 24, date_);
        put16(e + 26, uint16_t(cluster));
        put32(e + 28, size);
        raw.insert(raw.end(), e, e + 32);
    };
    if (idx != 0) {
        uint8_t dot[11], dotdot[11];
        std::memset(dot, ' ', 11);
        std::memset(dotdot, ' ', 11);
        dot[0] = '.';
        dotdot[0] = dotdot[1] = '.';
        entry(dot, 0x10, 0, d.chain.front(), 0);
        const uint32_t parentCluster = d.parent == 0 ? 0 : dirs_[size_t(d.parent)].chain.front();
        entry(dotdot, 0x10, 0, parentCluster, 0);
    }
    for (const Rec& r : d.recs) {
        if (r.needLfn) {
            const std::u16string w = utf8ToUtf16(r.longName);
            const size_t parts = (w.size() + 12) / 13;
            const uint8_t sum = lfnChecksum(r.shortName);
            for (size_t k = parts; k-- > 0;) {
                uint8_t e[32] = {};
                e[0] = uint8_t((k + 1) | (k + 1 == parts ? 0x40 : 0));
                e[11] = 0x0F;
                e[13] = sum;
                static const int pos[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
                for (int c = 0; c < 13; ++c) {
                    const size_t i = k * 13 + size_t(c);
                    const uint16_t ch = i < w.size() ? uint16_t(w[i]) : (i == w.size() ? 0x0000 : 0xFFFF);
                    put16(e + pos[c], ch);
                }
                raw.insert(raw.end(), e, e + 32);
            }
        }
        const uint32_t cl = r.dir >= 0 ? dirs_[size_t(r.dir)].chain.front() : r.cluster;
        entry(r.shortName, r.attr, r.caseFlags, cl, r.dir >= 0 ? 0 : r.size);
    }
    raw.resize(d.chain.size() * clusterBytes_, 0);
    for (size_t i = 0; i < d.chain.size(); ++i)
        t_.writeAt(clusterOffset(d.chain[i]), raw.data() + i * clusterBytes_, clusterBytes_);
}

void Fat32Writer::finish()
{
    if (finished_)
        return;
    // Size and place every directory.
    for (size_t i = 0; i < dirs_.size(); ++i) {
        DirNode& d = dirs_[i];
        size_t slots = i == 0 ? 0 : 2;
        for (const Rec& r : d.recs)
            slots += 1 + (r.needLfn ? (utf8ToUtf16(r.longName).size() + 12) / 13 : 0);
        const uint32_t need = std::max<uint32_t>(1, uint32_t((slots * 32 + clusterBytes_ - 1) / clusterBytes_));
        if (i == 0) {
            if (need > 1) {
                const uint32_t extra = allocate(need - 1);
                for (uint32_t k = 0; k < need - 1; ++k)
                    d.chain.push_back(extra + k);
            }
        } else {
            const uint32_t first = allocate(need);
            for (uint32_t k = 0; k < need; ++k)
                d.chain.push_back(first + k);
        }
        setChain(d.chain);
    }
    for (size_t i = 0; i < dirs_.size(); ++i)
        serializeDir(int(i));

    // FAT (both copies). Entries past fat_.size() are already zero from formatting.
    std::vector<uint8_t> fat(((fat_.size() * 4) + bps_ - 1) / bps_ * bps_, 0);
    for (size_t i = 0; i < fat_.size(); ++i)
        put32(fat.data() + i * 4, fat_[i]);
    for (int copy = 0; copy < 2; ++copy) {
        const uint64_t off = uint64_t(reserved_ + uint64_t(copy) * fatSectors_) * bps_;
        for (size_t pos = 0; pos < fat.size(); pos += 4 * MiB)
            t_.writeAt(off + pos, fat.data() + pos, std::min<size_t>(4 * MiB, fat.size() - pos));
    }
    // FSInfo with the real free count.
    std::vector<uint8_t> fsi(bps_, 0);
    put32(fsi.data(), 0x41615252);
    put32(fsi.data() + 484, 0x61417272);
    put32(fsi.data() + 488, clusters_ + 2 - next_);
    put32(fsi.data() + 492, next_);
    put32(fsi.data() + 508, 0xAA550000);
    t_.writeAt(uint64_t(bps_), fsi.data(), bps_);
    t_.writeAt(7ULL * bps_, fsi.data(), bps_);
    t_.flush();
    finished_ = true;
}

Fat32Info readFat32Info(BlockTarget& t)
{
    Fat32Info i;
    std::vector<uint8_t> bs(std::max<uint32_t>(t.sectorSize(), 512));
    t.readAt(0, bs.data(), bs.size());
    if (bs[510] != 0x55 || bs[511] != 0xAA || std::memcmp(bs.data() + 82, "FAT32", 5))
        return i;
    i.ok = true;
    i.bytesPerSector = get16(bs.data() + 11);
    i.sectorsPerCluster = bs[13];
    i.totalSectors = get32(bs.data() + 32);
    i.hiddenSectors = get32(bs.data() + 28);
    i.label = trim(std::string(reinterpret_cast<char*>(bs.data() + 71), 11));
    return i;
}

} // namespace bootaz
