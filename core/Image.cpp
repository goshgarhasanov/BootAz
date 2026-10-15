#include "Image.h"
#include "File.h"
#include "I18n.h"
#include "Iso.h"

#include <algorithm>
#include <cctype>
#include <cstring>

#ifdef BOOTAZ_HAVE_ZLIB
#include <zlib.h>
#endif
#ifdef BOOTAZ_HAVE_LZMA
#include <lzma.h>
#endif
#ifdef BOOTAZ_HAVE_ZSTD
#include <zstd.h>
#endif
#ifdef BOOTAZ_HAVE_BZIP2
#include <bzlib.h>
#endif

namespace bootaz {

namespace {

inline uint32_t le32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24); }
inline uint32_t be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
inline uint64_t be64(const uint8_t* p) { return (uint64_t(be32(p)) << 32) | be32(p + 4); }

Compression sniffCompression(const uint8_t* h, size_t n)
{
    if (n >= 2 && h[0] == 0x1f && h[1] == 0x8b) return Compression::Gzip;
    if (n >= 6 && !std::memcmp(h, "\xFD" "7zXZ\0", 6)) return Compression::Xz;
    if (n >= 4 && le32(h) == 0xFD2FB528) return Compression::Zstd;
    if (n >= 3 && h[0] == 'B' && h[1] == 'Z' && h[2] == 'h') return Compression::Bzip2;
    return Compression::None;
}

bool hasPartitionTable(const uint8_t* s0, const uint8_t* s1, bool& gpt)
{
    gpt = !std::memcmp(s1, "EFI PART", 8);
    if (s0[510] != 0x55 || s0[511] != 0xAA)
        return gpt;
    for (int i = 0; i < 4; ++i) {
        const uint8_t* e = s0 + 446 + 16 * i;
        if (e[4] != 0 && le32(e + 12) != 0)
            return true;
    }
    return gpt;
}

std::string fileNameOf(const std::string& path)
{
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

void nameLinux(IsoImage& iso, ImageInfo& info);

// Reads the Authenticode certificate table of a PE file and names who signed it.
void inspectEfiSignature(IsoImage& iso, ImageInfo& info)
{
    std::string loader;
    for (const auto& l : info.efiLoaders)
        if (endsWith(lower(l), "bootx64.efi"))
            loader = l;
    if (loader.empty() && !info.efiLoaders.empty())
        loader = info.efiLoaders.front();
    const int i = loader.empty() ? -1 : iso.find(loader);
    if (i < 0)
        return;
    std::vector<uint8_t> pe;
    try {
        pe = iso.readFile(iso.entries()[size_t(i)], 32 * MiB);
    } catch (...) {
        return;
    }
    if (pe.size() < 0x200 || pe[0] != 'M' || pe[1] != 'Z')
        return;
    const uint32_t peOff = le32(pe.data() + 0x3C);
    if (peOff + 0x100 > pe.size() || std::memcmp(pe.data() + peOff, "PE\0\0", 4))
        return;
    const uint16_t magic = uint16_t(pe[peOff + 24] | (pe[peOff + 25] << 8));
    const size_t dirBase = peOff + 24 + (magic == 0x20B ? 112 : 96);   // data directories
    const size_t secEntry = dirBase + 4 * 8;                             // IMAGE_DIRECTORY_ENTRY_SECURITY
    if (secEntry + 8 > pe.size())
        return;
    const uint32_t certOff = le32(pe.data() + secEntry), certLen = le32(pe.data() + secEntry + 4);
    if (!certOff || !certLen || uint64_t(certOff) + certLen > pe.size())
        return;
    info.efiSigned = true;
    const std::string certs(reinterpret_cast<const char*>(pe.data() + certOff), certLen);
    if (certs.find("Windows UEFI CA 2023") != std::string::npos)
        info.efiSigner = "Windows UEFI CA 2023";
    else if (certs.find("Microsoft Windows Production PCA 2011") != std::string::npos) {
        info.efiSigner = "Microsoft Windows Production PCA 2011";
        info.efiOldMicrosoftCa = true;
    } else if (certs.find("Microsoft UEFI CA 2023") != std::string::npos)
        info.efiSigner = "Microsoft UEFI CA 2023";
    else if (certs.find("Microsoft Corporation UEFI CA 2011") != std::string::npos)
        info.efiSigner = "Microsoft UEFI CA 2011";
    else
        info.efiSigner = "?";
}

void analyzeIso(IsoImage& iso, ImageInfo& info)
{
    info.label = iso.volumeLabel();
    info.fsTree = iso.treeName();
    for (const ElToritoEntry& b : iso.bootEntries()) {
        if (b.platform == 0xEF)
            info.elToritoEfi = true;
        else if (b.platform == 0 && b.bootable)
            info.elToritoBios = true;
    }

    iso.walk([&](const IsoEntry& e) {
        if (e.dir || e.symlink)
            return;
        ++info.fileCount;
        info.filesTotal += e.size;
        info.largestFile = std::max(info.largestFile, e.size);
        if (e.size >= 4 * GiB)
            info.over4GB.push_back(e.path);
        const std::string p = lower(e.path);
        if (startsWith(p, "/efi/boot/boot") && endsWith(p, ".efi") && std::count(p.begin(), p.end(), '/') == 3)
            info.efiLoaders.push_back(e.path);
        if (p == "/isolinux/isolinux.bin" || p == "/syslinux/isolinux.bin" || p == "/boot/isolinux/isolinux.bin" ||
            endsWith(p, "/ldlinux.c32") || endsWith(p, "/isolinux.cfg") || endsWith(p, "/syslinux.cfg"))
            info.hasSyslinux = true;
        if (endsWith(p, "/isolinux.cfg") || endsWith(p, "/syslinux.cfg") || endsWith(p, "/txt.cfg") ||
            endsWith(p, "/grub.cfg") || endsWith(p, "/loopback.cfg"))
            info.bootConfigs.push_back(e.path);
        if (endsWith(p, "/grub.cfg") || p.find("/i386-pc/") != std::string::npos)
            info.hasGrub = true;
        if (p == "/sources/install.wim" || p == "/sources/install.esd" || p == "/sources/install.swm") {
            info.isWindows = true;
            info.windowsInstallFile = e.path;
            info.windowsInstallSize = e.size;
        }
        if (p == "/sources/boot.wim" && iso.find("/bootmgr") >= 0)
            info.isWindows = true;
    });

    if (info.isWindows) {
        // The build branch in sources/idwbinfo.txt tells Windows 10 (vb, 19h1, rs*) from 11 (co, ni, ge, ...).
        std::string branch, arch;
        if (const int i = iso.find("/sources/idwbinfo.txt"); i >= 0) {
            const auto d = iso.readFile(iso.entries()[size_t(i)], 64 * 1024);
            for (const std::string& line : split(std::string(d.begin(), d.end()), '\n')) {
                const std::string l = lower(trim(line));
                if (startsWith(l, "buildbranch="))
                    branch = l.substr(12);
                if (startsWith(l, "buildarch="))
                    arch = l.substr(10);
            }
        }
        const std::string l = lower(info.label);
        if (!branch.empty())
            info.isWindows11 = !(startsWith(branch, "vb_") || startsWith(branch, "19h") || startsWith(branch, "rs") ||
                                 startsWith(branch, "th") || startsWith(branch, "win7") || startsWith(branch, "win8"));
        else
            info.isWindows11 = l.find("win11") != std::string::npos || l.find("w11") != std::string::npos;
        const bool ten = !branch.empty() && !info.isWindows11 && (startsWith(branch, "vb_") || startsWith(branch, "19h") ||
                                                                  startsWith(branch, "rs") || startsWith(branch, "th"));
        std::string a = arch == "amd64" || arch == "x64" ? "x64" : arch == "arm64" ? "ARM64" : arch == "x86" ? "x86" : "";
        if (a.empty())
            for (const auto& e : info.efiLoaders) {
                const std::string n = lower(e);
                if (endsWith(n, "bootx64.efi")) a = "x64";
                else if (endsWith(n, "bootaa64.efi") && a.empty()) a = "ARM64";
            }
        info.friendlyName = std::string("Windows") + (info.isWindows11 ? " 11" : ten ? " 10" : "") + (a.empty() ? "" : " " + a);
    }

    info.isoLabel = iso.isoLabel();
    auto has = [&](const char* p) { return iso.find(p) >= 0; };
    if (has("/casper")) {
        info.isLinux = true;
        info.distro = has("/casper/filesystem.squashfs") || has("/casper/minimal.squashfs") ? "Ubuntu" : "Ubuntu-based";
        if (has("/.disk/info")) {
            const auto d = iso.readFile(iso.entries()[size_t(iso.find("/.disk/info"))], 4096);
            const std::string s = trim(std::string(d.begin(), d.end()));
            if (!s.empty())
                info.distro = s.substr(0, s.find(' ') == std::string::npos ? s.size() : s.find('"'));
        }
        info.persistenceKind = "casper";
    } else if (has("/live") && (has("/live/filesystem.squashfs") || has("/live/vmlinuz"))) {
        info.isLinux = true;
        info.distro = "Debian Live";
        info.persistenceKind = "debian";
    } else if (has("/arch") || has("/arch/boot")) {
        info.isLinux = true;
        info.distro = "Arch Linux";
    } else if (has("/images/pxeboot") || has("/LiveOS")) {
        info.isLinux = true;
        info.distro = "Fedora / RHEL";
    } else if (has("/boot/x86_64/loader")) {
        info.isLinux = true;
        info.distro = "openSUSE";
    } else if (has("/apks") || has("/.alpine-release")) {
        info.isLinux = true;
        info.distro = "Alpine Linux";
    } else if (info.hasSyslinux || info.hasGrub) {
        info.isLinux = true;
        info.distro = "Linux";
    }
    if (info.isLinux)
        nameLinux(iso, info);
    inspectEfiSignature(iso, info);
    if (info.persistenceKind == "debian") {
        info.persistenceLabel = "persistence";
    } else if (info.persistenceKind == "casper") {
        // Ubuntu 23.04 and later look for "writable"; older releases for "casper-rw".
        const std::string v = info.friendlyName.substr(info.friendlyName.find_last_of(' ') + 1);
        const int major = std::atoi(v.c_str());
        info.persistenceLabel = major > 0 && major < 23 ? "casper-rw" : "writable";
    }
}

// "1.2.3" style version found in a label or description ("alpine-std 3.20.3" -> "3.20").
std::string versionIn(const std::string& s)
{
    for (size_t i = 0; i < s.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(s[i])) || (i && std::isalnum(static_cast<unsigned char>(s[i - 1]))))
            continue;
        size_t j = i;
        int dots = 0;
        while (j < s.size() && (std::isdigit(static_cast<unsigned char>(s[j])) || (s[j] == '.' && dots < 2))) {
            if (s[j] == '.')
                ++dots;
            ++j;
        }
        std::string v = s.substr(i, j - i);
        while (!v.empty() && v.back() == '.')
            v.pop_back();
        // keep "major.minor"
        const auto d1 = v.find('.');
        if (d1 != std::string::npos) {
            const auto d2 = v.find('.', d1 + 1);
            if (d2 != std::string::npos)
                v = v.substr(0, d2);
        }
        if (v.size() >= 2 || d1 != std::string::npos)
            return v;
    }
    return {};
}

void nameLinux(IsoImage& iso, ImageInfo& info)
{
    std::string text = info.label;
    if (const int i = iso.find("/.disk/info"); i >= 0) {
        const auto d = iso.readFile(iso.entries()[size_t(i)], 4096);
        text = trim(std::string(d.begin(), d.end())) + " " + text;
    }
    std::string name = info.distro;
    const std::string lt = lower(text);
    static const std::pair<const char*, const char*> known[] = {
        {"kubuntu", "Kubuntu"}, {"xubuntu", "Xubuntu"}, {"lubuntu", "Lubuntu"}, {"ubuntu", "Ubuntu"}, {"mint", "Linux Mint"},
        {"pop_os", "Pop!_OS"},   {"elementary", "elementary OS"}, {"zorin", "Zorin OS"}, {"kali", "Kali Linux"},
        {"debian", "Debian"},   {"fedora", "Fedora"},  {"arch", "Arch Linux"},  {"manjaro", "Manjaro"}, {"endeavour", "EndeavourOS"},
        {"opensuse", "openSUSE"}, {"alpine", "Alpine"}, {"rocky", "Rocky Linux"}, {"alma", "AlmaLinux"}, {"centos", "CentOS"},
        {"mx", "MX Linux"},     {"tails", "Tails"},    {"proxmox", "Proxmox"},   {"truenas", "TrueNAS"}, {"nixos", "NixOS"}};
    for (const auto& [key, nice] : known)
        if (lt.find(key) != std::string::npos) {
            name = nice;
            break;
        }
    if (name.empty())
        name = "Linux";
    const std::string ver = versionIn(text);
    info.friendlyName = ver.empty() || name == "Arch Linux" ? name : name + " " + ver;
}

} // namespace

std::string friendlyLabel(const ImageInfo& info, size_t maxChars)
{
    std::string n = info.friendlyName;
    if (n.empty())
        return {};
    if (maxChars <= 11) {
        // FAT: upper case, short words; Linux labels avoid spaces (kernel command lines).
        std::string s = upper(n);
        s = replaceAll(s, "WINDOWS ", "WIN");
        s = replaceAll(s, " LINUX", "");
        if (!info.isWindows) {
            s = replaceAll(s, " ", "_");
            if (s.size() > maxChars)
                s = replaceAll(s, ".", "");
        }
        if (s.size() > maxChars)
            s = s.substr(0, maxChars);
        while (!s.empty() && (s.back() == ' ' || s.back() == '_'))
            s.pop_back();
        return s;
    }
    if (!info.isWindows)
        n = replaceAll(n, " ", "_");
    return utf16ToUtf8(utf8ToUtf16(n).substr(0, maxChars));
}

bool ImageInfo::ddRecommended() const
{
    if (kind != ImageKind::Iso)
        return true;          // raw, VHD and compressed images can only be written as they are
    if (isWindows)
        return false;         // Windows media are always extracted
    if (!canIsoMode())
        return true;
    return isohybrid;         // Linux hybrids boot best exactly as the distribution built them
}

const char* compressionName(Compression c)
{
    switch (c) {
    case Compression::Gzip: return "gzip";
    case Compression::Xz: return "xz";
    case Compression::Zstd: return "zstd";
    case Compression::Bzip2: return "bzip2";
    default: return "-";
    }
}

bool compressionSupported(Compression c)
{
    switch (c) {
    case Compression::None: return true;
#ifdef BOOTAZ_HAVE_ZLIB
    case Compression::Gzip: return true;
#endif
#ifdef BOOTAZ_HAVE_LZMA
    case Compression::Xz: return true;
#endif
#ifdef BOOTAZ_HAVE_ZSTD
    case Compression::Zstd: return true;
#endif
#ifdef BOOTAZ_HAVE_BZIP2
    case Compression::Bzip2: return true;
#endif
    default: return false;
    }
}

Compression compressionFromName(const std::string& fileName)
{
    const std::string n = lower(fileName);
    if (endsWith(n, ".gz") || endsWith(n, ".gzip")) return Compression::Gzip;
    if (endsWith(n, ".xz")) return Compression::Xz;
    if (endsWith(n, ".zst") || endsWith(n, ".zstd")) return Compression::Zstd;
    if (endsWith(n, ".bz2")) return Compression::Bzip2;
    return Compression::None;
}

ImageInfo analyzeImage(const std::string& path)
{
    ImageInfo info;
    info.path = path;
    info.fileName = fileNameOf(path);
    File f(path, File::Read);
    info.fileSize = f.size();
    uint8_t head[1024] = {};
    const size_t n = f.readAt(0, head, sizeof head);

    info.compression = sniffCompression(head, n);
    if (info.compression != Compression::None) {
        info.kind = ImageKind::Compressed;
        if (!compressionSupported(info.compression))
            throw Error(format(T("This build cannot decompress %1 images"), {compressionName(info.compression)}));
        // gzip keeps the size (mod 4 GiB) at the end; xz/zstd sizes are found while writing.
        if (info.compression == Compression::Gzip && info.fileSize > 4) {
            uint8_t t[4];
            f.readExact(info.fileSize - 4, t, 4);
            const uint64_t isize = le32(t);
            uint64_t guess = isize;
            while (guess < info.fileSize)
                guess += 4 * GiB;   // compressed images are always smaller than their content
            info.dataSize = guess;
        }
        // Peek inside to tell an ISO from a raw disk image.
        auto s = ImageStream::open(path, info.compression);
        std::vector<uint8_t> peek(40 * 1024);
        size_t got = 0;
        while (got < peek.size()) {
            const size_t r = s->read(peek.data() + got, peek.size() - got);
            if (!r)
                break;
            got += r;
        }
        bool gpt = false;
        if (got >= 1024)
            info.isohybrid = hasPartitionTable(peek.data(), peek.data() + 512, gpt);
        info.gptInImage = gpt;
        if (got >= 0x8006 && !std::memcmp(peek.data() + 0x8001, "CD001", 5))
            info.fsTree = "ISO 9660";
        return info;
    }

    // Fixed-size VHD: raw disk followed by a 512-byte "conectix" footer.
    if (info.fileSize > 512) {
        uint8_t foot[512];
        f.readExact(info.fileSize - 512, foot, 512);
        if (!std::memcmp(foot, "conectix", 8)) {
            if (be32(foot + 60) != 2)
                throw Error(T("Only fixed-size VHD images can be written; convert dynamic VHDs first"));
            info.kind = ImageKind::Vhd;
            info.dataSize = std::min<uint64_t>(be64(foot + 48), info.fileSize - 512);
            bool gpt = false;
            info.isohybrid = hasPartitionTable(head, head + 512, gpt);
            info.gptInImage = gpt;
            return info;
        }
    }

    bool gpt = false;
    info.isohybrid = n >= 1024 && hasPartitionTable(head, head + 512, gpt);
    info.gptInImage = gpt;
    info.dataSize = info.fileSize;

    IsoImage iso;
    std::string why;
    if (iso.open(path, &why)) {
        info.kind = ImageKind::Iso;
        analyzeIso(iso, info);
        return info;
    }
    if (info.isohybrid) {
        info.kind = ImageKind::RawDisk;
        return info;
    }
    // Anything else can still be written byte for byte (e.g. a bare file system image).
    info.kind = ImageKind::RawDisk;
    return info;
}

// ---------------------------------------------------------------- streams

namespace {

class PlainStream final : public ImageStream {
public:
    PlainStream(const std::string& p, uint64_t limit) : f_(p, File::Read), limit_(limit ? limit : f_.size()) {}
    size_t read(void* buf, size_t len) override
    {
        if (pos_ >= limit_)
            return 0;
        len = size_t(std::min<uint64_t>(len, limit_ - pos_));
        const size_t n = f_.readAt(pos_, buf, len);
        pos_ += n;
        return n;
    }
    uint64_t sourcePosition() const override { return pos_; }

private:
    File f_;
    uint64_t limit_;
    uint64_t pos_ = 0;
};

// Base for decompressors: keeps an input buffer filled from the file.
class InputBuffer {
protected:
    explicit InputBuffer(const std::string& p) : f_(p, File::Read), in_(1 * MiB) {}
    size_t refill()
    {
        avail_ = f_.readAt(pos_, in_.data(), in_.size());
        pos_ += avail_;
        return avail_;
    }
    File f_;
    std::vector<uint8_t> in_;
    size_t avail_ = 0;
    uint64_t pos_ = 0;
};

#ifdef BOOTAZ_HAVE_ZLIB
class GzipStream final : public ImageStream, InputBuffer {
public:
    explicit GzipStream(const std::string& p) : InputBuffer(p)
    {
        if (inflateInit2(&z_, 16 + MAX_WBITS) != Z_OK)
            throw Error(T("Cannot start the gzip decoder"));
    }
    ~GzipStream() override { inflateEnd(&z_); }
    size_t read(void* buf, size_t len) override
    {
        z_.next_out = static_cast<Bytef*>(buf);
        z_.avail_out = uInt(len);
        while (z_.avail_out > 0 && !done_) {
            if (z_.avail_in == 0) {
                if (!refill())
                    break;
                z_.next_in = in_.data();
                z_.avail_in = uInt(avail_);
            }
            const int r = inflate(&z_, Z_NO_FLUSH);
            if (r == Z_STREAM_END) {
                // Concatenated gzip members (e.g. from pigz) continue after a reset.
                if (z_.avail_in > 0 || refillPeek()) {
                    inflateReset(&z_);
                } else {
                    done_ = true;
                }
            } else if (r != Z_OK && r != Z_BUF_ERROR) {
                throw Error(T("The gzip data is corrupt"));
            } else if (r == Z_BUF_ERROR && z_.avail_in == 0 && !refillPeek()) {
                throw Error(T("The gzip file ends too early"));
            }
        }
        return len - z_.avail_out;
    }
    uint64_t sourcePosition() const override { return pos_ - z_.avail_in; }

private:
    bool refillPeek()
    {
        if (!refill())
            return false;
        z_.next_in = in_.data();
        z_.avail_in = uInt(avail_);
        return true;
    }
    z_stream z_{};
    bool done_ = false;
};
#endif

#ifdef BOOTAZ_HAVE_LZMA
class XzStream final : public ImageStream, InputBuffer {
public:
    explicit XzStream(const std::string& p) : InputBuffer(p)
    {
        if (lzma_stream_decoder(&s_, UINT64_MAX, LZMA_CONCATENATED) != LZMA_OK)
            throw Error(T("Cannot start the xz decoder"));
    }
    ~XzStream() override { lzma_end(&s_); }
    size_t read(void* buf, size_t len) override
    {
        s_.next_out = static_cast<uint8_t*>(buf);
        s_.avail_out = len;
        while (s_.avail_out > 0 && !done_) {
            lzma_action action = LZMA_RUN;
            if (s_.avail_in == 0) {
                if (refill()) {
                    s_.next_in = in_.data();
                    s_.avail_in = avail_;
                } else {
                    action = LZMA_FINISH;
                }
            }
            const lzma_ret r = lzma_code(&s_, action);
            if (r == LZMA_STREAM_END)
                done_ = true;
            else if (r != LZMA_OK)
                throw Error(T("The xz data is corrupt"));
        }
        return len - s_.avail_out;
    }
    uint64_t sourcePosition() const override { return pos_ - s_.avail_in; }

private:
    lzma_stream s_ = LZMA_STREAM_INIT;
    bool done_ = false;
};
#endif

#ifdef BOOTAZ_HAVE_ZSTD
class ZstdStream final : public ImageStream, InputBuffer {
public:
    explicit ZstdStream(const std::string& p) : InputBuffer(p), d_(ZSTD_createDStream()) { ZSTD_initDStream(d_); }
    ~ZstdStream() override { ZSTD_freeDStream(d_); }
    size_t read(void* buf, size_t len) override
    {
        ZSTD_outBuffer out{buf, len, 0};
        while (out.pos < out.size) {
            if (ib_.pos == ib_.size) {
                if (!refill())
                    break;
                ib_ = {in_.data(), avail_, 0};
            }
            const size_t r = ZSTD_decompressStream(d_, &out, &ib_);
            if (ZSTD_isError(r))
                throw Error(T("The zstd data is corrupt"));
        }
        return out.pos;
    }
    uint64_t sourcePosition() const override { return pos_ - (ib_.size - ib_.pos); }

private:
    ZSTD_DStream* d_;
    ZSTD_inBuffer ib_{nullptr, 0, 0};
};
#endif

#ifdef BOOTAZ_HAVE_BZIP2
class Bzip2Stream final : public ImageStream, InputBuffer {
public:
    explicit Bzip2Stream(const std::string& p) : InputBuffer(p)
    {
        if (BZ2_bzDecompressInit(&s_, 0, 0) != BZ_OK)
            throw Error(T("Cannot start the bzip2 decoder"));
    }
    ~Bzip2Stream() override { BZ2_bzDecompressEnd(&s_); }
    size_t read(void* buf, size_t len) override
    {
        s_.next_out = static_cast<char*>(buf);
        s_.avail_out = unsigned(len);
        while (s_.avail_out > 0 && !done_) {
            if (s_.avail_in == 0) {
                if (!refill())
                    break;
                s_.next_in = reinterpret_cast<char*>(in_.data());
                s_.avail_in = unsigned(avail_);
            }
            const int r = BZ2_bzDecompress(&s_);
            if (r == BZ_STREAM_END) {
                if (s_.avail_in > 0 || refill()) {   // concatenated streams (pbzip2)
                    if (s_.avail_in == 0) {
                        s_.next_in = reinterpret_cast<char*>(in_.data());
                        s_.avail_in = unsigned(avail_);
                    }
                    BZ2_bzDecompressEnd(&s_);
                    char* keepIn = s_.next_in;
                    const unsigned keepAvail = s_.avail_in;
                    char* keepOut = s_.next_out;
                    const unsigned keepOutAvail = s_.avail_out;
                    s_ = bz_stream{};
                    BZ2_bzDecompressInit(&s_, 0, 0);
                    s_.next_in = keepIn;
                    s_.avail_in = keepAvail;
                    s_.next_out = keepOut;
                    s_.avail_out = keepOutAvail;
                } else {
                    done_ = true;
                }
            } else if (r != BZ_OK) {
                throw Error(T("The bzip2 data is corrupt"));
            }
        }
        return len - s_.avail_out;
    }
    uint64_t sourcePosition() const override { return pos_ - s_.avail_in; }

private:
    bz_stream s_{};
    bool done_ = false;
};
#endif

// ---- compressing sinks

class PlainSink final : public ImageSink {
public:
    explicit PlainSink(const std::string& p) : f_(p, File::Write) {}
    void write(const void* b, size_t n) override
    {
        f_.writeAt(pos_, b, n);
        pos_ += n;
    }
    void finish() override { f_.flush(); }

private:
    File f_;
    uint64_t pos_ = 0;
};

class OutputBuffer {
protected:
    explicit OutputBuffer(const std::string& p) : f_(p, File::Write), out_(1 * MiB) {}
    void emit(size_t n)
    {
        f_.writeAt(pos_, out_.data(), n);
        pos_ += n;
    }
    File f_;
    std::vector<uint8_t> out_;
    uint64_t pos_ = 0;
};

#ifdef BOOTAZ_HAVE_ZLIB
class GzipSink final : public ImageSink, OutputBuffer {
public:
    explicit GzipSink(const std::string& p) : OutputBuffer(p)
    {
        if (deflateInit2(&z_, 6, Z_DEFLATED, 16 + MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK)
            throw Error(T("Cannot start the gzip encoder"));
    }
    ~GzipSink() override { deflateEnd(&z_); }
    void write(const void* b, size_t n) override { pump(b, n, Z_NO_FLUSH); }
    void finish() override
    {
        pump(nullptr, 0, Z_FINISH);
        f_.flush();
    }

private:
    void pump(const void* b, size_t n, int flush)
    {
        z_.next_in = static_cast<Bytef*>(const_cast<void*>(b));
        z_.avail_in = uInt(n);
        int r;
        do {
            z_.next_out = out_.data();
            z_.avail_out = uInt(out_.size());
            r = deflate(&z_, flush);
            emit(out_.size() - z_.avail_out);
        } while (z_.avail_out == 0 || (flush == Z_FINISH && r != Z_STREAM_END));
    }
    z_stream z_{};
};
#endif

#ifdef BOOTAZ_HAVE_LZMA
class XzSink final : public ImageSink, OutputBuffer {
public:
    explicit XzSink(const std::string& p) : OutputBuffer(p)
    {
        lzma_mt mt{};
        mt.threads = std::max(1u, lzma_cputhreads());
        mt.preset = 3;
        mt.check = LZMA_CHECK_CRC64;
        if (lzma_stream_encoder_mt(&s_, &mt) != LZMA_OK && lzma_easy_encoder(&s_, 3, LZMA_CHECK_CRC64) != LZMA_OK)
            throw Error(T("Cannot start the xz encoder"));
    }
    ~XzSink() override { lzma_end(&s_); }
    void write(const void* b, size_t n) override { pump(b, n, LZMA_RUN); }
    void finish() override
    {
        pump(nullptr, 0, LZMA_FINISH);
        f_.flush();
    }

private:
    void pump(const void* b, size_t n, lzma_action a)
    {
        s_.next_in = static_cast<const uint8_t*>(b);
        s_.avail_in = n;
        lzma_ret r;
        do {
            s_.next_out = out_.data();
            s_.avail_out = out_.size();
            r = lzma_code(&s_, a);
            if (r != LZMA_OK && r != LZMA_STREAM_END)
                throw Error(T("xz compression failed"));
            emit(out_.size() - s_.avail_out);
        } while (s_.avail_in > 0 || (a == LZMA_FINISH && r != LZMA_STREAM_END));
    }
    lzma_stream s_ = LZMA_STREAM_INIT;
};
#endif

#ifdef BOOTAZ_HAVE_ZSTD
class ZstdSink final : public ImageSink, OutputBuffer {
public:
    explicit ZstdSink(const std::string& p) : OutputBuffer(p), c_(ZSTD_createCCtx())
    {
        ZSTD_CCtx_setParameter(c_, ZSTD_c_compressionLevel, 6);
        ZSTD_CCtx_setParameter(c_, ZSTD_c_nbWorkers, 4);
    }
    ~ZstdSink() override { ZSTD_freeCCtx(c_); }
    void write(const void* b, size_t n) override { pump(b, n, ZSTD_e_continue); }
    void finish() override
    {
        pump(nullptr, 0, ZSTD_e_end);
        f_.flush();
    }

private:
    void pump(const void* b, size_t n, ZSTD_EndDirective mode)
    {
        ZSTD_inBuffer in{b, n, 0};
        size_t left;
        do {
            ZSTD_outBuffer o{out_.data(), out_.size(), 0};
            left = ZSTD_compressStream2(c_, &o, &in, mode);
            if (ZSTD_isError(left))
                throw Error(T("zstd compression failed"));
            emit(o.pos);
        } while (mode == ZSTD_e_end ? left != 0 : in.pos < in.size);
    }
    ZSTD_CCtx* c_;
};
#endif

} // namespace

std::unique_ptr<ImageStream> ImageStream::open(const std::string& path, Compression c, uint64_t limit)
{
    switch (c) {
    case Compression::None: return std::make_unique<PlainStream>(path, limit);
#ifdef BOOTAZ_HAVE_ZLIB
    case Compression::Gzip: return std::make_unique<GzipStream>(path);
#endif
#ifdef BOOTAZ_HAVE_LZMA
    case Compression::Xz: return std::make_unique<XzStream>(path);
#endif
#ifdef BOOTAZ_HAVE_ZSTD
    case Compression::Zstd: return std::make_unique<ZstdStream>(path);
#endif
#ifdef BOOTAZ_HAVE_BZIP2
    case Compression::Bzip2: return std::make_unique<Bzip2Stream>(path);
#endif
    default: break;
    }
    throw Error(format(T("This build cannot decompress %1 images"), {compressionName(c)}));
}

std::unique_ptr<ImageSink> ImageSink::create(const std::string& path, Compression c)
{
    switch (c) {
    case Compression::None: return std::make_unique<PlainSink>(path);
#ifdef BOOTAZ_HAVE_ZLIB
    case Compression::Gzip: return std::make_unique<GzipSink>(path);
#endif
#ifdef BOOTAZ_HAVE_LZMA
    case Compression::Xz: return std::make_unique<XzSink>(path);
#endif
#ifdef BOOTAZ_HAVE_ZSTD
    case Compression::Zstd: return std::make_unique<ZstdSink>(path);
#endif
    default: break;
    }
    throw Error(format(T("This build cannot write %1 files"), {compressionName(c)}));
}

} // namespace bootaz
