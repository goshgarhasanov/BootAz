#include "Iso.h"
#include "I18n.h"

#include <algorithm>
#include <cstring>
#include <set>

namespace bootaz {

namespace {

constexpr uint32_t kSector = 2048;

inline uint16_t le16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
inline uint32_t le32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24); }
inline uint64_t le64(const uint8_t* p) { return le32(p) | (uint64_t(le32(p + 4)) << 32); }

std::string latin1ToUtf8(const uint8_t* p, size_t n)
{
    std::string s;
    for (size_t i = 0; i < n; ++i) {
        const uint8_t c = p[i];
        if (c < 0x80) {
            s += char(c);
        } else {
            s += char(0xc0 | (c >> 6));
            s += char(0x80 | (c & 0x3f));
        }
    }
    return s;
}

std::string ucs2beToUtf8(const uint8_t* p, size_t n)
{
    std::u16string w;
    for (size_t i = 0; i + 1 < n; i += 2)
        w += char16_t((p[i] << 8) | p[i + 1]);
    return utf16ToUtf8(w);
}

// UDF "d-characters" with a compression ID byte in front (8 = 8-bit, 16 = UCS-2 big endian).
std::string udfChars(const uint8_t* p, size_t n)
{
    if (n == 0)
        return {};
    if (p[0] == 16)
        return ucs2beToUtf8(p + 1, n - 1);
    return latin1ToUtf8(p + 1, n - 1);
}

// A dstring: the last byte holds the used length.
std::string udfDString(const uint8_t* p, size_t field)
{
    const size_t used = p[field - 1];
    if (used == 0 || used > field - 1)
        return {};
    return trim(udfChars(p, used));
}

std::string stripIsoVersion(std::string n)
{
    const auto semi = n.find(';');
    if (semi != std::string::npos)
        n.erase(semi);
    if (!n.empty() && n.back() == '.')
        n.pop_back();
    return n;
}

} // namespace

const char* IsoImage::treeName() const
{
    switch (tree_) {
    case Tree::Iso9660: return "ISO 9660";
    case Tree::RockRidge: return "ISO 9660 + Rock Ridge";
    case Tree::Joliet: return "ISO 9660 + Joliet";
    case Tree::Udf: return "UDF";
    default: return "-";
    }
}

// A name from the image must stay one plain path component: no "..", no separators, no control
// characters. A crafted image could otherwise make a copy land outside the target drive.
static bool safeName(const std::string& n)
{
    if (n.empty() || n == "." || n == ".." || n.size() > 1024)
        return false;
    for (unsigned char c : n)
        if (c < 0x20 || c == '/' || c == '\\')
            return false;
    return true;
}

int IsoImage::addEntry(IsoEntry e)
{
    if (entries_.size() >= 2000000)
        throw Error(T("The image has too many files"));
    entries_.push_back(std::move(e));
    return int(entries_.size() - 1);
}

int IsoImage::find(const std::string& path) const
{
    if (entries_.empty())
        return -1;
    int cur = 0;
    for (const std::string& part : split(path, '/')) {
        if (part.empty())
            continue;
        int next = -1;
        for (int c : entries_[size_t(cur)].children)
            if (iequals(entries_[size_t(c)].name, part)) {
                next = c;
                break;
            }
        if (next < 0)
            return -1;
        cur = next;
    }
    return cur;
}

void IsoImage::walk(const std::function<void(const IsoEntry&)>& fn) const
{
    std::function<void(int)> rec = [&](int i) {
        for (int c : entries_[size_t(i)].children) {
            fn(entries_[size_t(c)]);
            if (entries_[size_t(c)].dir)
                rec(c);
        }
    };
    if (!entries_.empty())
        rec(0);
}

bool IsoImage::open(const std::string& path, std::string* why)
{
    entries_.clear();
    boot_.clear();
    label_.clear();
    tree_ = Tree::None;
    hasUdf_ = hasIso_ = false;
    file_.open(path, File::Read);

    // Volume recognition sequence: ISO 9660 descriptors and UDF markers from sector 16 on.
    uint32_t elTorito = 0;
    for (uint32_t s = 16; s < 80; ++s) {
        uint8_t vd[kSector];
        if (file_.readAt(uint64_t(s) * kSector, vd, kSector) != kSector)
            break;
        if (!std::memcmp(vd + 1, "CD001", 5)) {
            hasIso_ = true;
            if (vd[0] == 0 && !std::memcmp(vd + 7, "EL TORITO SPECIFICATION", 23))
                elTorito = le32(vd + 0x47);
        } else if (!std::memcmp(vd + 1, "NSR02", 5) || !std::memcmp(vd + 1, "NSR03", 5)) {
            hasUdf_ = true;
        } else if (!std::memcmp(vd + 1, "BEA01", 5) || !std::memcmp(vd + 1, "TEA01", 5) ||
                   !std::memcmp(vd + 1, "BOOT2", 5) || !std::memcmp(vd + 1, "CDW02", 5)) {
            continue;
        } else if (hasIso_ || hasUdf_) {
            // Past the recognition area once a known descriptor was seen and this sector is blank.
            if (std::all_of(vd, vd + 8, [](uint8_t b) { return b == 0; }) && s > 20)
                break;
        }
    }
    if (!hasIso_ && !hasUdf_) {
        if (why)
            *why = T("Not an ISO 9660 or UDF image");
        return false;
    }
    if (elTorito)
        readElTorito(elTorito);

    for (uint32_t s = 16; s < 32 && hasIso_; ++s) {
        uint8_t vd[kSector];
        if (file_.readAt(uint64_t(s) * kSector, vd, kSector) != kSector)
            break;
        if (vd[0] == 1 && !std::memcmp(vd + 1, "CD001", 5)) {
            pvdLabel_ = trim(latin1ToUtf8(vd + 40, 32));
            break;
        }
    }
    if (hasUdf_ && readUdf() && entries_.size() > 1) {
        tree_ = Tree::Udf;
        return true;
    }
    entries_.clear();
    if (hasIso_ && readIso9660(why))
        return true;
    if (why && why->empty())
        *why = T("The image has no readable file system");
    return false;
}

// ---------------------------------------------------------------- ISO 9660

bool IsoImage::readIso9660(std::string* why)
{
    std::vector<uint8_t> pvd, joliet;
    for (uint32_t s = 16; s < 80; ++s) {
        uint8_t vd[kSector];
        if (file_.readAt(uint64_t(s) * kSector, vd, kSector) != kSector)
            break;
        if (std::memcmp(vd + 1, "CD001", 5))
            continue;
        if (vd[0] == 255)
            break;
        if (vd[0] == 1 && pvd.empty())
            pvd.assign(vd, vd + kSector);
        if (vd[0] == 2 && vd[88] == '%' && vd[89] == '/' && (vd[90] == '@' || vd[90] == 'C' || vd[90] == 'E'))
            joliet.assign(vd, vd + kSector);
    }
    if (pvd.empty()) {
        if (why)
            *why = T("The ISO 9660 primary volume descriptor is missing");
        return false;
    }
    pvdLabel_ = trim(latin1ToUtf8(pvd.data() + 40, 32));
    if (label_.empty())
        label_ = pvdLabel_;

    // Rock Ridge: the root "." record carries a SUSP "SP" entry.
    const uint8_t* rootRec = pvd.data() + 156;
    const uint32_t rootLba = le32(rootRec + 2), rootSize = le32(rootRec + 10);
    bool rockRidge = false;
    {
        uint8_t sec[kSector];
        file_.readExact(uint64_t(rootLba) * kSector, sec, kSector);
        const uint8_t len = sec[0], nameLen = sec[32];
        size_t su = 33 + nameLen + ((nameLen % 2 == 0) ? 1 : 0);
        if (len > su + 7 && sec[su] == 'S' && sec[su + 1] == 'P' && sec[su + 4] == 0xBE && sec[su + 5] == 0xEF) {
            rockRidge = true;
            rrSkip_ = sec[su + 6];
        }
    }

    IsoEntry root;
    root.dir = true;
    root.path = "/";
    addEntry(root);
    if (rockRidge) {
        tree_ = Tree::RockRidge;
        parseIsoDir(0, rootLba, rootSize, false, true, 0);
    } else if (!joliet.empty()) {
        tree_ = Tree::Joliet;
        const uint8_t* jr = joliet.data() + 156;
        const std::string jl = trim(ucs2beToUtf8(joliet.data() + 40, 32));
        if (!jl.empty())
            label_ = jl;
        parseIsoDir(0, le32(jr + 2), le32(jr + 10), true, false, 0);
    } else {
        tree_ = Tree::Iso9660;
        parseIsoDir(0, rootLba, rootSize, false, false, 0);
    }
    return true;
}

void IsoImage::parseIsoDir(int parent, uint32_t lba, uint32_t size, bool joliet, bool rockRidge, int depth)
{
    if (depth > 64 || size > 64 * MiB)
        return;
    std::vector<uint8_t> data(size);
    data.resize(file_.readAt(uint64_t(lba) * kSector, data.data(), size));

    IsoEntry pending;     // multi-extent file being assembled
    bool havePending = false;
    size_t pos = 0;
    while (pos < data.size()) {
        const uint8_t len = data[pos];
        if (len == 0) {   // records never cross sectors: jump to the next one
            pos = (pos / kSector + 1) * kSector;
            continue;
        }
        if (pos + len > data.size() || len < 34)
            break;
        const uint8_t* r = data.data() + pos;
        pos += len;
        const uint8_t nameLen = r[32];
        if (nameLen == 1 && (r[33] == 0 || r[33] == 1))
            continue;   // "." and ".."

        IsoEntry e;
        e.parent = parent;
        e.dir = (r[25] & 0x02) != 0;
        const bool more = (r[25] & 0x80) != 0;
        uint32_t extLba = le32(r + 2);
        uint32_t extLen = le32(r + 10);
        e.name = joliet ? ucs2beToUtf8(r + 33, nameLen) : latin1ToUtf8(r + 33, nameLen);
        e.name = stripIsoVersion(e.name);
        if (joliet) {
            // Joliet names keep the ";1" too.
        }

        bool relocated = false;
        if (rockRidge) {
            std::string rrName;
            bool isLink = false;
            std::string target;
            // System Use area, possibly continued through "CE" entries.
            std::vector<uint8_t> su(r + 33 + nameLen + ((nameLen % 2 == 0) ? 1 : 0), r + len);
            if (su.size() >= size_t(rrSkip_))
                su.erase(su.begin(), su.begin() + rrSkip_);
            for (int hops = 0; hops < 8 && !su.empty(); ++hops) {
                std::vector<uint8_t> next;
                size_t i = 0;
                while (i + 4 <= su.size()) {
                    const uint8_t* s = su.data() + i;
                    const uint8_t sl = s[2];
                    if (sl < 4 || i + sl > su.size())
                        break;
                    if (s[0] == 'N' && s[1] == 'M' && sl >= 5) {
                        if (!(s[4] & 0x06))
                            rrName += latin1ToUtf8(s + 5, sl - 5);
                    } else if (s[0] == 'P' && s[1] == 'X' && sl >= 12) {
                        if ((le32(s + 4) & 0170000) == 0120000)
                            isLink = true;
                    } else if (s[0] == 'S' && s[1] == 'L' && sl >= 5) {
                        isLink = true;
                        size_t c = 5;
                        while (c + 2 <= sl) {
                            const uint8_t cf = s[c], cl = s[c + 1];
                            std::string comp;
                            if (cf & 0x02) comp = ".";
                            else if (cf & 0x04) comp = "..";
                            else if (cf & 0x08) comp = "";
                            else comp = latin1ToUtf8(s + c + 2, std::min<size_t>(cl, sl - c - 2));
                            if (!target.empty() && target.back() != '/')
                                target += '/';
                            target += (cf & 0x08) ? "/" : comp;
                            c += 2 + cl;
                        }
                    } else if (s[0] == 'R' && s[1] == 'E') {
                        relocated = true;
                    } else if (s[0] == 'C' && s[1] == 'L' && sl >= 12) {
                        // Deep directory moved elsewhere: its real contents live at this LBA.
                        extLba = le32(s + 4);
                        uint8_t sec[kSector];
                        if (file_.readAt(uint64_t(extLba) * kSector, sec, kSector) == kSector)
                            extLen = le32(sec + 10);
                        e.dir = true;
                    } else if (s[0] == 'C' && s[1] == 'E' && sl >= 28) {
                        const uint32_t ceLba = le32(s + 4), ceOff = le32(s + 12), ceLen = le32(s + 20);
                        if (ceLen < 64 * KiB) {
                            next.resize(ceLen);
                            next.resize(file_.readAt(uint64_t(ceLba) * kSector + ceOff, next.data(), ceLen));
                        }
                    } else if (s[0] == 'S' && s[1] == 'T') {
                        break;
                    }
                    i += sl;
                }
                su.swap(next);
            }
            if (!rrName.empty())
                e.name = rrName;
            if (isLink) {
                e.symlink = true;
                e.linkTarget = target;
                e.dir = false;
            }
        }
        if (relocated || !safeName(e.name))
            continue;

        if (havePending && (pending.name != e.name || e.dir)) {
            // A multi-extent run ended without its final record; keep what we have.
            pending.path = entries_[size_t(parent)].path + (parent == 0 ? "" : "/") + pending.name;
            const int idx = addEntry(pending);
            entries_[size_t(parent)].children.push_back(idx);
            havePending = false;
        }
        if (!e.dir && !e.symlink) {
            if (!havePending) {
                pending = e;
                pending.size = 0;
                pending.extents.clear();
                havePending = true;
            }
            pending.extents.push_back({uint64_t(extLba) * kSector, extLen});
            pending.size += extLen;
            if (more)
                continue;
            e = pending;
            havePending = false;
        }

        e.path = (parent == 0 ? "/" : entries_[size_t(parent)].path + "/") + e.name;
        const int idx = addEntry(e);
        entries_[size_t(parent)].children.push_back(idx);
        if (e.dir)
            parseIsoDir(idx, extLba, extLen, joliet, rockRidge, depth + 1);
    }
}

// ---------------------------------------------------------------- El Torito

void IsoImage::readElTorito(uint32_t catalogLba)
{
    uint8_t cat[kSector];
    if (file_.readAt(uint64_t(catalogLba) * kSector, cat, kSector) != kSector)
        return;
    if (cat[0] != 1 || cat[30] != 0x55 || cat[31] != 0xAA)
        return;
    auto entry = [&](const uint8_t* p, uint8_t platform) {
        ElToritoEntry e;
        e.platform = platform;
        e.bootable = p[0] == 0x88;
        e.media = p[1] & 0x0f;
        e.sectors512 = le16(p + 6);
        e.lba = le32(p + 8);
        boot_.push_back(e);
    };
    entry(cat + 32, cat[1]);
    size_t off = 64;
    while (off + 32 <= kSector) {
        const uint8_t hdr = cat[off];
        if (hdr != 0x90 && hdr != 0x91)
            break;
        const uint8_t platform = cat[off + 1];
        const uint16_t count = le16(cat + off + 2);
        off += 32;
        for (uint16_t i = 0; i < count && off + 32 <= kSector; ++i, off += 32)
            if (cat[off] == 0x88 || cat[off] == 0x00)
                entry(cat + off, platform);
        if (hdr == 0x91)
            break;
    }
}

// ---------------------------------------------------------------- UDF

uint64_t IsoImage::udfBlockToOffset(uint16_t partRef, uint32_t lbn)
{
    if (partRef >= udfMaps_.size())
        throw Error(T("Bad UDF partition reference"));
    const UdfMap& m = udfMaps_[partRef];
    if (m.metadata) {
        uint64_t logical = uint64_t(lbn) * udfBlock_;
        for (const IsoExtent& x : udfMetaExtents_) {
            if (logical < x.length)
                return x.offset + logical;
            logical -= x.length;
        }
        throw Error(T("UDF metadata block out of range"));
    }
    for (const UdfPartition& p : udfParts_)
        if (p.number == m.partition)
            return (uint64_t(p.start) + lbn) * udfBlock_;
    throw Error(T("UDF partition not found"));
}

// Reads a File Entry / Extended File Entry and fills in size, type and data extents.
bool IsoImage::udfReadEntry(uint16_t partRef, uint32_t lbn, IsoEntry& e, uint8_t& fileType, int depth)
{
    std::vector<uint8_t> b(udfBlock_);
    if (file_.readAt(udfBlockToOffset(partRef, lbn), b.data(), b.size()) != b.size())
        return false;
    const uint16_t tag = le16(b.data());
    if (tag != 261 && tag != 266)
        return false;
    fileType = b[27];
    const uint16_t icbFlags = le16(b.data() + 34);
    const int adType = icbFlags & 7;
    e.size = le64(b.data() + 56);
    uint32_t lEa, lAd;
    size_t adStart;
    if (tag == 261) {
        lEa = le32(b.data() + 168);
        lAd = le32(b.data() + 172);
        adStart = 176 + lEa;
    } else {
        lEa = le32(b.data() + 208);
        lAd = le32(b.data() + 212);
        adStart = 216 + lEa;
    }
    if (adStart + lAd > b.size())
        return false;

    if (adType == 3) {   // data stored in the entry itself
        e.inlineData.assign(b.begin() + long(adStart), b.begin() + long(adStart + lAd));
        e.inlineData.resize(std::min<uint64_t>(e.size, e.inlineData.size()));
        return true;
    }

    std::vector<uint8_t> ads(b.begin() + long(adStart), b.begin() + long(adStart + lAd));
    uint64_t remaining = e.size;
    for (int guard = 0; guard < 4096 && !ads.empty(); ++guard) {
        std::vector<uint8_t> next;
        const size_t step = adType == 0 ? 8 : adType == 1 ? 16 : 20;
        for (size_t i = 0; i + step <= ads.size(); i += step) {
            const uint8_t* a = ads.data() + i;
            const uint32_t raw = le32(a);
            const uint32_t len = raw & 0x3fffffff;
            const uint32_t kind = raw >> 30;
            if (len == 0)
                break;
            uint32_t pos;
            uint16_t ref = partRef;
            if (adType == 0) {
                pos = le32(a + 4);
            } else if (adType == 1) {
                pos = le32(a + 4);
                ref = le16(a + 8);
            } else {   // ext_ad
                pos = le32(a + 12);
                ref = le16(a + 16);
            }
            if (kind == 3) {   // next block of allocation descriptors
                std::vector<uint8_t> ext(udfBlock_);
                if (file_.readAt(udfBlockToOffset(ref, pos), ext.data(), ext.size()) == ext.size() && le16(ext.data()) == 258) {
                    const uint32_t l = std::min<uint32_t>(le32(ext.data() + 20), udfBlock_ - 24);
                    next.assign(ext.begin() + 24, ext.begin() + 24 + l);
                }
                break;
            }
            const uint64_t use = std::min<uint64_t>(len, remaining);
            if (use == 0)
                continue;
            e.extents.push_back({kind == 0 ? udfBlockToOffset(ref, pos) : IsoExtent::kSparse, use});
            remaining -= use;
        }
        ads.swap(next);
    }
    (void)depth;
    return true;
}

void IsoImage::udfParseDir(int parent, const IsoEntry& dirEntry, uint16_t partRef, int depth)
{
    if (depth > 64)
        return;
    const std::vector<uint8_t> d = entryData(dirEntry, 64 * MiB);
    size_t pos = 0;
    while (pos + 38 <= d.size()) {
        const uint8_t* f = d.data() + pos;
        if (le16(f) != 257)
            break;
        const uint8_t chars = f[18];
        const uint8_t lFi = f[19];
        const uint32_t icbLbn = le32(f + 24);
        const uint16_t icbRef = le16(f + 28);
        const uint16_t lIu = le16(f + 36);
        const size_t total = (38 + lIu + lFi + 3) & ~size_t(3);
        if (pos + 38 + lIu + lFi > d.size())
            break;
        const std::string name = udfChars(f + 38 + lIu, lFi);
        pos += total;
        if (chars & 0x08)   // parent
            continue;
        if (chars & 0x04)   // deleted
            continue;
        if (!safeName(name))
            continue;

        IsoEntry e;
        e.parent = parent;
        e.name = name;
        uint8_t type = 0;
        if (!udfReadEntry(icbRef, icbLbn, e, type, depth))
            continue;
        e.dir = (chars & 0x02) || type == 4;
        if (type == 12) {
            e.symlink = true;
            e.dir = false;
        }
        e.path = (parent == 0 ? "/" : entries_[size_t(parent)].path + "/") + e.name;
        const int idx = addEntry(e);
        entries_[size_t(parent)].children.push_back(idx);
        if (entries_[size_t(idx)].dir) {
            const IsoEntry copy = entries_[size_t(idx)];
            udfParseDir(idx, copy, icbRef, depth + 1);
        }
    }
    (void)partRef;
}

bool IsoImage::readUdf()
{
    try {
        udfParts_.clear();
        udfMaps_.clear();
        udfMetaExtents_.clear();
        uint8_t avdp[kSector];
        if (file_.readAt(256ULL * kSector, avdp, kSector) != kSector || le16(avdp) != 2)
            return false;
        const uint32_t vdsLen = le32(avdp + 16), vdsLoc = le32(avdp + 20);

        uint32_t fsdLbn = 0;
        uint16_t fsdRef = 0;
        std::string lvLabel;
        for (uint32_t i = 0; i < std::min<uint32_t>(vdsLen / kSector, 64); ++i) {
            uint8_t d[kSector];
            if (file_.readAt(uint64_t(vdsLoc + i) * kSector, d, kSector) != kSector)
                break;
            const uint16_t tag = le16(d);
            if (tag == 8)
                break;
            if (tag == 5) {
                UdfPartition p;
                p.number = le16(d + 22);
                p.start = le32(d + 188);
                p.length = le32(d + 192);
                udfParts_.push_back(p);
            } else if (tag == 6) {
                udfBlock_ = le32(d + 212);
                if (udfBlock_ != 512 && udfBlock_ != 1024 && udfBlock_ != 2048 && udfBlock_ != 4096)
                    udfBlock_ = 2048;
                lvLabel = udfDString(d + 84, 128);
                fsdLbn = le32(d + 248 + 4);
                fsdRef = le16(d + 248 + 8);
                const uint32_t nMaps = le32(d + 268);
                size_t m = 440;
                for (uint32_t k = 0; k < nMaps && m + 2 <= kSector; ++k) {
                    UdfMap map;
                    map.type = d[m];
                    const uint8_t mlen = d[m + 1];
                    if (map.type == 1) {
                        map.partition = le16(d + m + 4);
                    } else if (map.type == 2) {
                        const std::string ident(reinterpret_cast<const char*>(d + m + 5), 23);
                        map.partition = le16(d + m + 38);
                        if (ident.rfind("*UDF Metadata Partition", 0) == 0 || ident.find("Metadata") != std::string::npos) {
                            map.metadata = true;
                            map.metaFileLoc = le32(d + m + 40);
                        }
                    }
                    udfMaps_.push_back(map);
                    if (mlen == 0)
                        break;
                    m += mlen;
                }
            }
        }
        if (udfParts_.empty() || udfMaps_.empty())
            return false;

        // Metadata partition (UDF 2.50+): its contents are the extents of the metadata file.
        for (size_t i = 0; i < udfMaps_.size(); ++i) {
            if (!udfMaps_[i].metadata)
                continue;
            uint16_t phys = 0;
            for (size_t k = 0; k < udfMaps_.size(); ++k)
                if (!udfMaps_[k].metadata && udfMaps_[k].partition == udfMaps_[i].partition)
                    phys = uint16_t(k);
            IsoEntry meta;
            uint8_t type = 0;
            if (!udfReadEntry(phys, udfMaps_[i].metaFileLoc, meta, type, 0))
                return false;
            udfMetaExtents_ = meta.extents;
            udfMetaPhysRef_ = phys;
        }

        std::vector<uint8_t> fsd(udfBlock_);
        if (file_.readAt(udfBlockToOffset(fsdRef, fsdLbn), fsd.data(), fsd.size()) != fsd.size() || le16(fsd.data()) != 256)
            return false;
        const uint32_t rootLbn = le32(fsd.data() + 400 + 4);
        const uint16_t rootRef = le16(fsd.data() + 400 + 8);

        IsoEntry root;
        root.dir = true;
        root.path = "/";
        uint8_t type = 0;
        if (!udfReadEntry(rootRef, rootLbn, root, type, 0))
            return false;
        addEntry(root);
        const IsoEntry copy = entries_[0];
        udfParseDir(0, copy, rootRef, 0);
        if (!lvLabel.empty())
            label_ = lvLabel;
        return true;
    } catch (const Error&) {
        return false;
    }
}

// ---------------------------------------------------------------- data

std::vector<uint8_t> IsoImage::entryData(const IsoEntry& e, uint64_t maxBytes)
{
    if (!e.inlineData.empty() || e.extents.empty())
        return e.inlineData;
    std::vector<uint8_t> out;
    out.reserve(size_t(std::min<uint64_t>(e.size, maxBytes)));
    streamFile(e, [&](const uint8_t* p, size_t n) {
        if (out.size() + n > maxBytes)
            throw Error(T("File too large to read into memory"));
        out.insert(out.end(), p, p + n);
    });
    return out;
}

std::vector<uint8_t> IsoImage::readFile(const IsoEntry& e, uint64_t maxBytes) { return entryData(e, maxBytes); }

void IsoImage::streamFile(const IsoEntry& e, const std::function<void(const uint8_t*, size_t)>& sink, const JobContext* ctx)
{
    if (!e.inlineData.empty()) {
        sink(e.inlineData.data(), e.inlineData.size());
        return;
    }
    std::vector<uint8_t> buf(4 * MiB);
    for (const IsoExtent& x : e.extents) {
        uint64_t done = 0;
        while (done < x.length) {
            if (ctx)
                ctx->checkCancel();
            const size_t n = size_t(std::min<uint64_t>(buf.size(), x.length - done));
            if (x.offset == IsoExtent::kSparse) {
                std::fill(buf.begin(), buf.begin() + long(n), 0);
            } else if (file_.readAt(x.offset + done, buf.data(), n) != n) {
                throw Error(format(T("The image is truncated: %1"), {e.path}));
            }
            sink(buf.data(), n);
            done += n;
        }
    }
}

std::vector<uint8_t> IsoImage::readRaw(uint64_t offset, size_t len)
{
    std::vector<uint8_t> v(len);
    v.resize(file_.readAt(offset, v.data(), len));
    return v;
}

} // namespace bootaz
