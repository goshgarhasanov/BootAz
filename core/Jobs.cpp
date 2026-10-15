#include "Jobs.h"
#include "BootCode.h"
#include "Fat32.h"
#include "File.h"
#include "I18n.h"
#include "Iso.h"
#include "TreeWriter.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <future>
#include <random>
#include <thread>

namespace fs = std::filesystem;

namespace bootaz {

namespace {

constexpr size_t kChunk = 8 * MiB;

fs::path u8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }
std::string fromPath(const fs::path& p)
{
    const auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

std::string lowerFs(FsType f)
{
    switch (f) {
    case FsType::Fat32: return "fat32";
    case FsType::Ntfs: return "ntfs";
    case FsType::Exfat: return "exfat";
    case FsType::Ext2: return "ext2";
    case FsType::Ext3: return "ext3";
    case FsType::Ext4: return "ext4";
    }
    return "fat32";
}

PartType partTypeFor(FsType f)
{
    switch (f) {
    case FsType::Fat32: return PartType::Fat32;
    case FsType::Ntfs: return PartType::Ntfs;
    case FsType::Exfat: return PartType::Exfat;
    default: return PartType::Linux;
    }
}

std::string cleanLabel(const std::string& label, FsType f)
{
    std::string l = trim(label);
    if (l.empty())
        l = "BOOTAZ";
    switch (f) {
    case FsType::Fat32: return trim(fat32Label(l));
    case FsType::Exfat: return utf16ToUtf8(utf8ToUtf16(l).substr(0, 15));
    case FsType::Ntfs: return utf16ToUtf8(utf8ToUtf16(l).substr(0, 32));
    default: return l.substr(0, 16);
    }
}

// Throttles progress reports and adds the speed.
class Meter {
public:
    Meter(const JobContext& ctx, std::string stage, uint64_t total) : ctx_(ctx), stage_(std::move(stage)), total_(total)
    {
        start_ = last_ = std::chrono::steady_clock::now();
    }
    void update(uint64_t done, bool force = false)
    {
        const auto now = std::chrono::steady_clock::now();
        if (!force && now - last_ < std::chrono::milliseconds(200))
            return;
        last_ = now;
        const double secs = std::chrono::duration<double>(now - start_).count();
        Progress p{stage_, done, total_, secs > 0.2 ? double(done) / secs : 0};
        if (ctx_.onProgress)
            ctx_.onProgress(p);
    }
    void setTotal(uint64_t t) { total_ = t; }

private:
    const JobContext& ctx_;
    std::string stage_;
    uint64_t total_;
    std::chrono::steady_clock::time_point start_, last_;
};

#ifndef _WIN32
std::string hostSyslinuxVersion()
{
    if (findProgram("syslinux").empty())
        return {};
    std::string out;
    JobContext quiet;
    try {
        runProgram({"syslinux", "--version"}, quiet, &out);
    } catch (...) {
        return {};
    }
    // "syslinux 6.04  Copyright ..."
    const auto p = out.find("syslinux ");
    if (p == std::string::npos)
        return {};
    return trim(out.substr(p + 9, out.find(' ', p + 9) - p - 9));
}

bool hostGrubBios()
{
    return !findProgram("grub-install").empty() &&
           (fs::exists("/usr/lib/grub/i386-pc/boot.img") || fs::exists("/usr/lib/grub2/i386-pc/boot.img"));
}
#endif

// "ISOLINUX 6.04 ..." inside isolinux.bin -> "6.04".
std::string isoSyslinuxVersion(IsoImage& iso)
{
    std::vector<std::string> bins;
    iso.walk([&](const IsoEntry& e) {
        if (!e.dir && iequals(e.name, "isolinux.bin"))
            bins.push_back(e.path);
    });
    for (const std::string& p : bins) {
        const int i = iso.find(p);
        if (i < 0)
            continue;
        const auto d = iso.readFile(iso.entries()[size_t(i)], 1 * MiB);
        const std::string s(d.begin(), d.end());
        const auto at = s.find("ISOLINUX ");
        if (at != std::string::npos)
            return s.substr(at + 9, s.find(' ', at + 9) - at - 9);
    }
    return {};
}

std::string architectureOf(const ImageInfo& img)
{
    bool x64 = false, arm = false, x86 = false;
    for (const std::string& l : img.efiLoaders) {
        const std::string n = lower(l);
        if (endsWith(n, "bootx64.efi")) x64 = true;
        if (endsWith(n, "bootaa64.efi")) arm = true;
        if (endsWith(n, "bootia32.efi")) x86 = true;
    }
    if (x64) return "amd64";
    if (arm) return "arm64";
    if (x86) return "x86";
    return "amd64";
}

// Boot configs that find the boot drive by its label must see the drive's new label.
std::string patchLabel(const std::string& text, const std::string& oldLabel, const std::string& newLabel)
{
    if (oldLabel.empty() || oldLabel == newLabel)
        return text;
    std::string t = replaceAll(text, oldLabel, newLabel);
    if (oldLabel.find(' ') != std::string::npos) {
        t = replaceAll(t, replaceAll(oldLabel, " ", "\\x20"), newLabel);
        t = replaceAll(t, replaceAll(oldLabel, " ", "\\040"), newLabel);
    }
    return t;
}

// Adds the kernel option that switches on persistence to every live boot entry of a config file.
std::string patchPersistence(const std::string& text, const std::string& kind)
{
    const std::string key = kind == "casper" ? "boot=casper" : "boot=live";
    const std::string add = kind == "casper" ? " persistent" : " persistence";
    std::string out;
    for (const std::string& line : split(text, '\n')) {
        std::string l = line;
        if (l.find(key) != std::string::npos && l.find(add.substr(1)) == std::string::npos) {
            const auto p = l.find(key) + key.size();
            l.insert(p, add);
        }
        out += l + "\n";
    }
    if (!out.empty() && (text.empty() || text.back() != '\n'))
        out.pop_back();
    return out;
}

} // namespace

const char* fsName(FsType f)
{
    switch (f) {
    case FsType::Fat32: return "FAT32";
    case FsType::Ntfs: return "NTFS";
    case FsType::Exfat: return "exFAT";
    case FsType::Ext2: return "ext2";
    case FsType::Ext3: return "ext3";
    case FsType::Ext4: return "ext4";
    }
    return "?";
}

bool parseFs(const std::string& s, FsType& out)
{
    const std::string k = lower(s);
    if (k == "fat32" || k == "vfat" || k == "fat") out = FsType::Fat32;
    else if (k == "ntfs") out = FsType::Ntfs;
    else if (k == "exfat") out = FsType::Exfat;
    else if (k == "ext2") out = FsType::Ext2;
    else if (k == "ext3") out = FsType::Ext3;
    else if (k == "ext4") out = FsType::Ext4;
    else return false;
    return true;
}

const char* targetName(TargetSystem t)
{
    switch (t) {
    case TargetSystem::Uefi: return "UEFI";
    case TargetSystem::Bios: return "BIOS";
    case TargetSystem::BiosUefi: return "BIOS+UEFI";
    }
    return "?";
}

bool parseTarget(const std::string& s, TargetSystem& out)
{
    const std::string k = lower(s);
    if (k == "uefi") out = TargetSystem::Uefi;
    else if (k == "bios" || k == "legacy") out = TargetSystem::Bios;
    else if (k == "bios+uefi" || k == "both" || k == "uefi+bios") out = TargetSystem::BiosUefi;
    else return false;
    return true;
}

std::vector<FsType> availableFileSystems()
{
    std::vector<FsType> v{FsType::Fat32};
    for (FsType f : {FsType::Ntfs, FsType::Exfat, FsType::Ext2, FsType::Ext3, FsType::Ext4})
        if (osCanFormat(lowerFs(f)))
            v.push_back(f);
    return v;
}

// ---------------------------------------------------------------- planning

Plan planWrite(const DriveInfo& drive, const WriteOptions& opt)
{
    Plan p;
    p.options = opt;
    WriteOptions& o = p.options;
    if (drive.system)
        p.errors.push_back(T("This drive holds the running operating system and cannot be written"));
    if (drive.readOnly)
        p.errors.push_back(T("The drive is write protected"));
    if (drive.size < 32 * MiB && !drive.isFile)
        p.errors.push_back(T("The drive is too small"));

    const bool haveImage = !o.image.empty();
    if (haveImage) {
        try {
            p.image = analyzeImage(o.image);
        } catch (const Error& e) {
            p.errors.push_back(e.what());
            return p;
        }
        if (o.mode == WriteMode::Auto)
            p.dd = p.image.ddRecommended();
        else
            p.dd = o.mode == WriteMode::Dd;
        if (!p.dd && !p.image.canIsoMode()) {
            p.errors.push_back(T("This image can only be written in DD mode"));
            p.dd = true;
        }
    }

    if (p.dd) {
        o.mode = WriteMode::Dd;
        p.biosLoader = "image";
        if (p.image.dataSize && p.image.dataSize > drive.size)
            p.errors.push_back(format(T("The image (%1) is larger than the drive (%2)"),
                                      {humanBytes(p.image.dataSize), humanBytes(drive.size)}));
        if (p.image.isWindows)
            p.warnings.push_back(T("Windows install images do not boot when written in DD mode; ISO mode is recommended"));
        if (o.persistenceBytes)
            p.warnings.push_back(T("Persistence is only available in ISO mode and was turned off"));
        o.persistenceBytes = 0;
        return p;
    }
    if (haveImage)
        o.mode = WriteMode::Iso;

    // ---- file system
    const ImageInfo& img = p.image;
    if (o.winLayout == WinLayout::Split)
        o.splitWim = true;
    const bool winBig = img.isWindows && img.windowsInstallSize >= 4 * GiB;
#ifndef _WIN32
    const bool haveWimlib = !findProgram("wimlib-imagex").empty();
#else
    const bool haveWimlib = !findProgram("wimlib-imagex.exe").empty();
#endif
    if (o.fsAuto) {
        if (!haveImage)
            o.fs = drive.size <= 32 * GiB ? FsType::Fat32 : (osCanFormat("exfat") ? FsType::Exfat : FsType::Ntfs);
        else if (winBig && o.splitWim && haveWimlib && img.over4GB.size() == 1)
            o.fs = FsType::Fat32;
        else if (!img.over4GB.empty())
            o.fs = FsType::Ntfs;
        else
            o.fs = FsType::Fat32;
    }
    if (o.fs != FsType::Fat32 && !osCanFormat(lowerFs(o.fs)))
        p.errors.push_back(format(T("This computer cannot create %1 file systems"), {fsName(o.fs)}));
    if (haveImage && (o.fs == FsType::Ext2 || o.fs == FsType::Ext3 || o.fs == FsType::Ext4))
        p.errors.push_back(T("Bootable drives need FAT32, NTFS or exFAT"));
    if (o.fs == FsType::Fat32 && !img.over4GB.empty()) {
        const bool onlyWim = winBig && img.over4GB.size() == 1;
        if (onlyWim && o.splitWim && haveWimlib) {
            p.warnings.push_back(T("install.wim is larger than 4 GB and will be split into parts for FAT32"));
        } else if (onlyWim && o.splitWim) {
            p.errors.push_back(T("Splitting install.wim needs wimlib (wimlib-imagex), which is not installed"));
        } else {
            p.errors.push_back(format(T("The image has files larger than 4 GB (%1); FAT32 cannot store them. Choose NTFS or exFAT."),
                                      {img.over4GB.front()}));
        }
    }
    if (img.isWindows && winBig && o.winLayout == WinLayout::Dual) {
        // Microsoft's own layout: boot files on FAT32 (any UEFI, Secure Boot), everything on NTFS.
        if (!osCanFormat("ntfs"))
            p.errors.push_back(format(T("This computer cannot create %1 file systems"), {"NTFS"}));
        p.dualPartition = true;
        o.fs = FsType::Fat32;
        o.splitWim = false;
        p.errors.erase(std::remove_if(p.errors.begin(), p.errors.end(),
                                      [](const std::string& e) { return e.find("FAT32") != std::string::npos && e.find("4 GB") != std::string::npos; }),
                       p.errors.end());
    }
    if (!(o.fs == FsType::Fat32 && winBig && o.splitWim))
        o.splitWim = false;
    if (o.label.empty() && haveImage) {
        // Name the drive after the system it installs ("WIN11 X64", "UBUNTU_2404").
        const size_t maxChars = o.fs == FsType::Fat32 ? 11 : o.fs == FsType::Exfat ? 15 : 32;
        o.label = friendlyLabel(img, maxChars);
        if (o.label.empty())
            o.label = img.label;
    }
    o.label = cleanLabel(o.label, o.fs);

    // ---- partition scheme and boot
    if (o.scheme == Scheme::Gpt && o.target != TargetSystem::Uefi) {
        p.warnings.push_back(T("GPT drives boot with UEFI only; the target was set to UEFI"));
        o.target = TargetSystem::Uefi;
    }
    if (haveImage) {
        const bool wantUefi = o.target != TargetSystem::Bios;
        const bool wantBios = o.target != TargetSystem::Uefi;
        if (wantUefi && !img.uefiBootable())
            p.warnings.push_back(T("The image has no UEFI boot loader; it may only boot on BIOS computers"));
        if (wantUefi && img.efiOldMicrosoftCa)
            p.warnings.push_back(T("The Windows boot loader is signed only with Microsoft's 2011 certificate. PCs that received the 2024+ Secure Boot update will refuse it; use a newer ISO or turn off Secure Boot if that happens."));
        if (wantUefi && img.efiLoaders.empty() && img.elToritoEfi)
            p.warnings.push_back(T("UEFI boot of this image needs its El Torito EFI image; DD mode is the safer choice"));
        p.uefiNtfs = wantUefi && (o.fs == FsType::Ntfs || o.fs == FsType::Exfat) && img.uefiBootable() && !p.dualPartition;
        if (wantBios) {
            if (img.isWindows) {
                if (o.fs == FsType::Exfat)
                    p.errors.push_back(T("Windows cannot boot from exFAT on BIOS computers; choose NTFS or FAT32"));
                else
                    p.biosLoader = "bootmgr";
            } else {
#ifndef _WIN32
                const std::string host = hostSyslinuxVersion();
                IsoImage iso;
                std::string isoVer;
                if (iso.open(o.image))
                    isoVer = isoSyslinuxVersion(iso);
                if (img.hasSyslinux && o.fs == FsType::Fat32 && !host.empty() && !isoVer.empty()) {
                    p.biosLoader = "syslinux";
                    if (host.substr(0, 3) != isoVer.substr(0, 3))
                        p.warnings.push_back(format(T("The image uses Syslinux %1 but this computer has %2; if BIOS boot fails, use DD mode"),
                                                    {isoVer, host}));
                } else if (img.hasGrub && hostGrubBios()) {
                    p.biosLoader = "grub";
                }
#endif
                if (p.biosLoader.empty()) {
                    if (o.target == TargetSystem::Bios)
                        p.errors.push_back(T("This image cannot be made BIOS bootable in ISO mode; use DD mode"));
                    else
                        p.warnings.push_back(T("BIOS boot is not available for this image in ISO mode; the drive will boot with UEFI only"));
                }
            }
        }
    }

    // ---- extras
    if (o.persistenceBytes) {
        if (img.persistenceKind.empty()) {
            p.warnings.push_back(T("This image does not support persistence; the option was turned off"));
            o.persistenceBytes = 0;
        } else if (!osCanFormat("ext4")) {
            p.errors.push_back(T("Persistence needs an ext4 partition, which this computer cannot create"));
        } else if (o.fs != FsType::Fat32) {
            p.errors.push_back(T("Persistence needs FAT32 as the main file system"));
        } else if (o.scheme == Scheme::Mbr && false) {
        }
    }
    if (img.isWindows) {
        o.windows.architecture = architectureOf(img);
        if (!o.windows.localAccount.empty() && !validAccountName(o.windows.localAccount))
            p.errors.push_back(T("The account name is not valid for Windows"));
        if (!o.windows.locale.empty() && !validLocale(o.windows.locale))
            p.errors.push_back(T("The regional format is not valid (for example az-Latn-AZ)"));
        if (!o.windows.computerName.empty() && !validComputerName(o.windows.computerName))
            p.errors.push_back(T("The computer name is not valid (up to 15 letters, digits or hyphens)"));
    } else {
        o.windows = WindowsOptions{};
    }

    // ---- sizes
    const uint64_t usable = layoutEnd(drive.size, o.scheme, drive.sectorSize ? drive.sectorSize : 512) - layoutStart() -
                            (p.uefiNtfs ? MiB : 0) - o.persistenceBytes;
    if (int64_t(usable) <= int64_t(64 * MiB))
        p.errors.push_back(T("The drive is too small for these settings"));
    else if (haveImage && img.filesTotal + img.filesTotal / 50 + 32 * MiB > usable)
        p.errors.push_back(format(T("The image needs %1 but only %2 is available on the drive"),
                                  {humanBytes(img.filesTotal), humanBytes(usable)}));
    if (o.badBlockPasses > 0)
        p.warnings.push_back(format(T("The bad block test writes the whole drive %1 time(s) and takes a long time"),
                                    {std::to_string(o.badBlockPasses * 1)}));
    return p;
}

// ---------------------------------------------------------------- DD mode

namespace {

void writeDd(Disk& disk, const Plan& plan, const JobContext& ctx)
{
    const ImageInfo& img = plan.image;
    ctx.log(format(T("Writing %1 in DD mode"), {img.fileName}));
    // Old backup GPT at the end of the drive would confuse firmware after a smaller image.
    {
        std::vector<uint8_t> zero(MiB, 0);
        const uint64_t tail = disk.size() > MiB ? (disk.size() - MiB) / disk.sectorSize() * disk.sectorSize() : 0;
        disk.writeAt(tail, zero.data(), size_t(disk.size() - tail));
    }
    auto in = ImageStream::open(img.path, img.compression, img.kind == ImageKind::Vhd ? img.dataSize : 0);
    const bool compressed = img.compression != Compression::None;
    Meter meter(ctx, T("Writing image"), compressed ? img.fileSize : img.dataSize);
    auto hasher = Hasher::create(HashAlgo::Sha256);
    std::vector<uint8_t> buf(kChunk);
    uint64_t written = 0, sinceFlush = 0;
    const uint32_t ss = disk.sectorSize();
    for (;;) {
        ctx.checkCancel();
        size_t n = 0;
        while (n < buf.size()) {   // fill the buffer: decoders may return short reads
            const size_t r = in->read(buf.data() + n, buf.size() - n);
            if (!r)
                break;
            n += r;
        }
        if (!n)
            break;
        hasher->update(buf.data(), n);
        size_t out = n;
        if (out % ss) {   // last block: pad to a whole sector
            out = (out + ss - 1) / ss * ss;
            std::fill(buf.begin() + long(n), buf.begin() + long(out), 0);
        }
        if (written + out > disk.size())
            throw Error(format(T("The image is larger than the drive (%1)"), {humanBytes(disk.size())}));
        disk.writeAt(written, buf.data(), out);
        written += n;
        sinceFlush += n;
        if (sinceFlush >= 256 * MiB) {   // keep the progress honest on drives with big caches
            disk.flush();
            sinceFlush = 0;
        }
        meter.update(compressed ? in->sourcePosition() : written);
    }
    meter.update(compressed ? img.fileSize : written, true);
    ctx.log(T("Flushing the drive cache"));
    disk.flush();
    ctx.log(format(T("%1 written"), {humanBytes(written)}));
    const std::string digest = hasher->finishHex();

    if (plan.options.verify) {
        ctx.log(T("Verifying what was written"));
        Meter vm(ctx, T("Verifying"), written);
        auto vh = Hasher::create(HashAlgo::Sha256);
        for (uint64_t done = 0; done < written;) {
            ctx.checkCancel();
            const size_t n = size_t(std::min<uint64_t>(buf.size(), written - done));
            const size_t aligned = (n + ss - 1) / ss * ss;
            disk.readAt(done, buf.data(), aligned);
            vh->update(buf.data(), n);
            done += n;
            vm.update(done);
        }
        vm.update(written, true);
        if (vh->finishHex() != digest)
            throw Error(T("Verification failed: the data read back from the drive differs from the image. The drive may be faulty or fake."));
        ctx.log(T("Verification passed"));
    }
}

// ---------------------------------------------------------------- bad blocks

BadBlockReport badBlocksOn(Disk& disk, int passes, const JobContext& ctx)
{
    static const uint8_t patterns[4] = {0x55, 0xAA, 0xFF, 0x00};
    BadBlockReport r;
    const uint64_t size = disk.size() / disk.sectorSize() * disk.sectorSize();
    r.testedBytes = size;
    const uint32_t ss = disk.sectorSize();
    std::vector<uint8_t> buf(kChunk), got(kChunk);
    uint64_t aliased = 0;
    passes = std::clamp(passes, 1, 4);

    for (int pass = 0; pass < passes; ++pass) {
        const uint8_t pat = patterns[pass];
        // Every sector carries its own number, so a drive that wraps around (fake capacity)
        // returns the wrong sector instead of just the wrong bytes.
        auto fill = [&](uint64_t off, size_t n, uint8_t* p) {
            std::memset(p, pat, n);
            for (size_t s = 0; s < n; s += ss) {
                const uint64_t sector = (off + s) / ss;
                for (int k = 0; k < 8; ++k)
                    p[s + size_t(k)] = uint8_t((sector >> (8 * k)) ^ pat);
            }
        };
        Meter wm(ctx, format(T("Bad block test: pass %1 of %2, writing"), {std::to_string(pass + 1), std::to_string(passes)}), size);
        for (uint64_t off = 0; off < size;) {
            ctx.checkCancel();
            const size_t n = size_t(std::min<uint64_t>(buf.size(), size - off));
            fill(off, n, buf.data());
            try {
                disk.writeAt(off, buf.data(), n);
            } catch (const Error&) {
                r.badSectors += n / ss;
                if (r.badRanges.size() < 64)
                    r.badRanges.push_back({off, off + n});
            }
            off += n;
            wm.update(off);
        }
        disk.flush();
        Meter rm(ctx, format(T("Bad block test: pass %1 of %2, reading"), {std::to_string(pass + 1), std::to_string(passes)}), size);
        for (uint64_t off = 0; off < size;) {
            ctx.checkCancel();
            const size_t n = size_t(std::min<uint64_t>(buf.size(), size - off));
            fill(off, n, buf.data());
            bool readOk = true;
            try {
                disk.readAt(off, got.data(), n);
            } catch (const Error&) {
                readOk = false;
            }
            for (size_t s = 0; s < n; s += ss) {
                if (readOk && !std::memcmp(buf.data() + s, got.data() + s, ss))
                    continue;
                ++r.badSectors;
                const uint64_t at = off + s;
                if (r.badRanges.empty() || r.badRanges.back().second != at) {
                    if (r.badRanges.size() < 64)
                        r.badRanges.push_back({at, at + ss});
                } else {
                    r.badRanges.back().second = at + ss;
                }
                if (readOk) {
                    // The sector holds another sector's stamp: the drive maps two addresses to the
                    // same flash, which is how fake-capacity drives behave.
                    uint64_t stored = 0;
                    for (int k = 0; k < 8; ++k)
                        stored |= uint64_t(uint8_t(got[s + size_t(k)] ^ pat)) << (8 * k);
                    if (stored != at / ss && stored < size / ss &&
                        !std::memcmp(got.data() + s + 8, buf.data() + s + 8, ss - 8))
                        ++aliased;
                }
            }
            off += n;
            rm.update(off);
        }
        if (r.badSectors)
            break;   // no point in more passes
    }
    if (aliased) {
        // Writes wrapped around: only the sectors that kept their own data are real.
        r.fakeCapacity = true;
        r.realCapacity = size - std::min(size, r.badSectors * ss);
    } else if (r.badRanges.size() == 1 && r.badRanges[0].second >= size && r.badRanges[0].first > 0 &&
               r.badRanges[0].second - r.badRanges[0].first >= size / 10) {
        // Everything past one point is lost: a drive that silently drops writes beyond its real size.
        r.fakeCapacity = true;
        r.realCapacity = r.badRanges[0].first;
    }
    return r;
}

// ---------------------------------------------------------------- ISO mode

struct CopyPlan {
    IsoImage* iso = nullptr;
    const Plan* plan = nullptr;
    TreeWriter* writer = nullptr;
    std::string splitDir;   // folder with install*.swm when install.wim is split
    std::function<bool(const IsoEntry&)> include;   // copy only these (empty = everything)
    bool wants(const IsoEntry& e) const { return !include || include(e); }
};

void copyTree(CopyPlan& cp, const JobContext& ctx)
{
    const Plan& plan = *cp.plan;
    IsoImage& iso = *cp.iso;
    const std::string persist = plan.options.persistenceBytes ? plan.image.persistenceKind : "";
    const bool syslinux = plan.biosLoader == "syslinux";

    uint64_t total = 0;
    iso.walk([&](const IsoEntry& e) {
        if (!e.dir && !e.symlink && cp.wants(e))
            total += e.size;
    });
    Meter meter(ctx, T("Copying files"), total);
    uint64_t done = 0;
    int skippedLinks = 0;

    auto isSplitWim = [&](const IsoEntry& e) { return !cp.splitDir.empty() && iequals(e.path, plan.image.windowsInstallFile); };

    std::function<void(int)> rec = [&](int idx) {
        for (int c : iso.entries()[size_t(idx)].children) {
            const IsoEntry& e = iso.entries()[size_t(c)];
            ctx.checkCancel();
            if (e.symlink) {
                ++skippedLinks;
                continue;
            }
            if (!cp.wants(e))
                continue;
            if (e.dir) {
                cp.writer->makeDir(e.path);
                rec(c);
                continue;
            }
            if (isSplitWim(e)) {
                // Copy the parts made by wimlib instead.
                const std::string dir = e.path.substr(0, e.path.rfind('/'));
                for (const auto& part : fs::directory_iterator(u8(cp.splitDir))) {
                    const std::string name = fromPath(part.path().filename());
                    const uint64_t sz = uint64_t(part.file_size());
                    File src(fromPath(part.path()), File::Read);
                    cp.writer->writeFile(dir + "/" + name, sz, [&](const FileSink& sink) {
                        std::vector<uint8_t> b(4 * MiB);
                        for (uint64_t off = 0; off < sz;) {
                            ctx.checkCancel();
                            const size_t n = src.readAt(off, b.data(), size_t(std::min<uint64_t>(b.size(), sz - off)));
                            sink(b.data(), n);
                            off += n;
                            meter.update(done + off * e.size / std::max<uint64_t>(sz, 1) / 2);
                        }
                    });
                }
                done += e.size;
                continue;
            }
            const std::string lname = lower(e.name);
            const bool isConfig = e.size < 1 * MiB && (endsWith(lname, ".cfg") || endsWith(lname, ".conf"));
            const bool relabel = isConfig && plan.image.isLinux && !plan.image.isoLabel.empty() && plan.image.isoLabel != plan.options.label;
            std::string patched;
            bool changed = false;
            if (isConfig && (!persist.empty() || relabel)) {
                const auto d = iso.readFile(e);
                const std::string original(d.begin(), d.end());
                patched = original;
                if (relabel)
                    patched = patchLabel(patched, plan.image.isoLabel, plan.options.label);
                if (!persist.empty())
                    patched = patchPersistence(patched, persist);
                changed = patched != original;
                if (changed)
                    ctx.log(format(T("Updated %1 for the new drive"), {e.path}));
            }
            if (changed) {
                cp.writer->writeFile(e.path, patched.size(), [&](const FileSink& sink) {
                    sink(reinterpret_cast<const uint8_t*>(patched.data()), patched.size());
                });
                if (syslinux && lname == "isolinux.cfg")
                    cp.writer->writeFile(e.path.substr(0, e.path.size() - 12) + "syslinux.cfg", patched.size(), [&](const FileSink& sink) {
                        sink(reinterpret_cast<const uint8_t*>(patched.data()), patched.size());
                    });
                done += e.size;
                continue;
            }
            cp.writer->writeFile(e.path, e.size, [&](const FileSink& sink) {
                uint64_t local = 0;
                iso.streamFile(e, [&](const uint8_t* p, size_t n) {
                    sink(p, n);
                    local += n;
                    meter.update(done + local);
                }, &ctx);
            });
            if (syslinux && lname == "isolinux.cfg" && iso.find(e.path.substr(0, e.path.size() - 12) + "syslinux.cfg") < 0) {
                const auto d = iso.readFile(e);
                cp.writer->writeFile(e.path.substr(0, e.path.size() - 12) + "syslinux.cfg", d.size(), [&](const FileSink& sink) {
                    sink(d.data(), d.size());
                });
            }
            done += e.size;
            meter.update(done);
        }
    };
    rec(0);
    meter.update(total, true);
    if (skippedLinks)
        ctx.log(format(T("%1 symbolic link(s) skipped (not needed to boot)"), {std::to_string(skippedLinks)}));
}

void verifyTree(CopyPlan& cp, const JobContext& ctx)
{
    IsoImage& iso = *cp.iso;
    const bool persist = cp.plan->options.persistenceBytes > 0;
    uint64_t total = 0;
    iso.walk([&](const IsoEntry& e) {
        if (!e.dir && !e.symlink && cp.wants(e))
            total += e.size;
    });
    Meter meter(ctx, T("Verifying"), total);
    uint64_t done = 0;
    iso.walk([&](const IsoEntry& e) {
        if (e.dir || e.symlink || !cp.wants(e))
            return;
        ctx.checkCancel();
        const std::string ln = lower(e.name);
        const bool relabel = cp.plan->image.isLinux && !cp.plan->image.isoLabel.empty() && cp.plan->image.isoLabel != cp.plan->options.label;
        if (((persist || relabel) && e.size < MiB && (endsWith(ln, ".cfg") || endsWith(ln, ".conf"))) ||
            (!cp.splitDir.empty() && iequals(e.path, cp.plan->image.windowsInstallFile))) {
            done += e.size;   // changed on purpose
            return;
        }
        auto a = Hasher::create(HashAlgo::Sha256);
        auto b = Hasher::create(HashAlgo::Sha256);
        iso.streamFile(e, [&](const uint8_t* p, size_t n) { a->update(p, n); }, &ctx);
        uint64_t local = 0;
        cp.writer->readBack(e.path, e.size, [&](const uint8_t* p, size_t n) {
            b->update(p, n);
            local += n;
            meter.update(done + local);
        });
        if (a->finishHex() != b->finishHex())
            throw Error(format(T("Verification failed: %1 differs from the image"), {e.path}));
        done += e.size;
    });
    meter.update(total, true);
    ctx.log(T("Verification passed"));
}

// autorun.inf: the drive's name and icon in Windows Explorer.
void writeAutorun(TreeWriter& w, const std::string& label)
{
    const std::string inf = "[autorun]\r\nlabel=" + label + "\r\nicon=autorun.ico\r\n";
    w.writeFile("/autorun.inf", inf.size(), [&](const FileSink& s) { s(reinterpret_cast<const uint8_t*>(inf.data()), inf.size()); });
}

} // namespace

// Defined in the generated autorunIcon.cpp (BootAz icon for autorun.inf).
const unsigned char* autorunIconData();
size_t autorunIconSize();

void runWrite(const DriveInfo& selected, const Plan& plan, const JobContext& ctx)
{
    if (!plan.errors.empty())
        throw Error(plan.errors.front());
    const WriteOptions& o = plan.options;

    // The drive must still be the one the user picked.
    DriveInfo drive;
    if (!findDrive(selected.path, drive, selected.isFile))
        throw Error(T("The drive was removed"));
    if (!o.expectedId.empty() && drive.id != o.expectedId)
        throw Error(T("The drive changed since it was selected; refresh the list and try again"));

    ctx.log(format(T("Target: %1"), {drive.displayName()}));
    auto disk = Disk::open(drive, true, ctx);

    if (o.badBlockPasses > 0) {
        const BadBlockReport r = badBlocksOn(*disk, o.badBlockPasses, ctx);
        if (r.fakeCapacity)
            throw Error(format(T("FAKE DRIVE: it reports %1 but only about %2 can really be stored"),
                               {humanBytes(drive.size), humanBytes(r.realCapacity)}));
        if (r.badSectors)
            throw Error(format(T("%1 bad sector(s) found; the drive is not reliable"), {std::to_string(r.badSectors)}));
        ctx.log(T("No bad blocks found"));
    }

    if (plan.dd) {
        writeDd(*disk, plan, ctx);
        disk->release(o.eject, ctx);
        ctx.log(T("Done"));
        return;
    }

    IsoImage iso;
    if (!o.image.empty() && !iso.open(o.image))
        throw Error(T("Cannot read the image"));
    // Windows "dual" layout: boot files on FAT32, the whole image on NTFS (Microsoft's method).
    auto bootOnly = [](const IsoEntry& e) {
        const std::string p = lower(e.path);
        return !startsWith(p, "/sources") || p == "/sources" || p == "/sources/boot.wim";
    };
    uint64_t bootBytes = 0;
    if (plan.dualPartition)
        iso.walk([&](const IsoEntry& e) {
            if (!e.dir && !e.symlink && bootOnly(e))
                bootBytes += e.size;
        });

    // ---- partitions
    const uint32_t ss = disk->sectorSize();
    const uint64_t start = layoutStart();
    const uint64_t end = layoutEnd(disk->size(), o.scheme, ss);
    Layout layout;
    layout.scheme = o.scheme;
    uint64_t mainEnd = end - (plan.uefiNtfs ? MiB : 0) - o.persistenceBytes / MiB * MiB;
    PartSpec main;
    main.offset = start;
    main.size = mainEnd - start;
    PartSpec data;   // dual layout only
    if (plan.dualPartition) {
        main.size = std::max<uint64_t>(512 * MiB, (bootBytes + bootBytes / 20 + 96 * MiB + MiB - 1) / MiB * MiB);
        data.offset = main.offset + main.size;
        data.size = mainEnd - data.offset;
        data.type = PartType::Ntfs;
        data.name = friendlyLabel(plan.image, 32).empty() ? o.label : friendlyLabel(plan.image, 32);
    }
    main.type = partTypeFor(o.fs);
    main.name = o.label;
    main.active = o.target != TargetSystem::Uefi;
    layout.parts.push_back(main);
    if (plan.dualPartition)
        layout.parts.push_back(data);
    int persistIndex = 0, uefiIndex = 0;
    if (o.persistenceBytes) {
        PartSpec ps;
        ps.offset = mainEnd;
        ps.size = (end - (plan.uefiNtfs ? MiB : 0)) - mainEnd;
        ps.type = PartType::Linux;
        ps.name = plan.image.persistenceLabel.empty() ? "persistence" : plan.image.persistenceLabel;
        layout.parts.push_back(ps);
        persistIndex = int(layout.parts.size());
    }
    if (plan.uefiNtfs) {
        PartSpec u;
        u.offset = end - MiB;
        u.size = MiB;
        u.type = PartType::Esp;
        u.name = "UEFI:NTFS";
        layout.parts.push_back(u);
        uefiIndex = int(layout.parts.size());
    }
    std::vector<uint8_t> mbrCode;
    if (o.scheme == Scheme::Mbr) {
        if (plan.biosLoader == "syslinux")
            mbrCode = mbrSyslinux();
        else
            mbrCode = mbrWindows();   // boots the active partition (or explains that nothing is bootable)
    }
    if (!o.quickFormat) {
        Meter zm(ctx, T("Erasing the drive"), disk->size());
        std::vector<uint8_t> zero(kChunk, 0);
        for (uint64_t off = 0; off < disk->size();) {
            ctx.checkCancel();
            const size_t n = size_t(std::min<uint64_t>(zero.size(), disk->size() - off));
            disk->writeAt(off, zero.data(), n);
            off += n;
            zm.update(off);
        }
    }
    wipeSignatures(*disk, ctx);
    ctx.log(format(T("Creating a %1 partition table"), {schemeName(o.scheme)}));
    writePartitionTable(*disk, layout, mbrCode);
    disk->rescanPartitions(int(layout.parts.size()), ctx);

    // ---- file system
    std::unique_ptr<TreeWriter> writer;
    std::unique_ptr<BlockTarget> mainTarget;
    std::string mountPoint;
    if (o.fs == FsType::Fat32) {
        mainTarget = disk->partition(1, main.offset, main.size, ctx);
        Fat32Params fp;
        fp.label = o.label;
        fp.clusterSize = o.clusterSize;
        fp.hiddenSectors = uint32_t(main.offset / ss);
        writer = std::make_unique<Fat32Writer>(*mainTarget, fp, ctx);
    } else {
        ctx.log(format(T("Formatting %1"), {fsName(o.fs)}));
        ctx.progress(T("Formatting"), 0, 0);
        osFormat(*disk, 1, main.offset, main.size, lowerFs(o.fs), o.label, o.clusterSize, ctx);
        if (!o.image.empty() || o.autorun) {
            mountPoint = disk->mountPartition(1, main.offset, lowerFs(o.fs), ctx);
            writer = std::make_unique<DirTreeWriter>(mountPoint, main.size, false);
        }
    }

    // ---- files
    CopyPlan cp;
    std::string splitTmp;
    if (!o.image.empty()) {
        cp.iso = &iso;
        if (plan.dualPartition)
            cp.include = bootOnly;
        cp.plan = &plan;
        cp.writer = writer.get();
        if (o.splitWim) {
            // wimlib needs the WIM as a file: extract it to a temporary folder and split it there.
            const fs::path tmp = u8(makePrivateTempDir("bootaz-wim"));
            splitTmp = fromPath(tmp);
            const int wi = iso.find(plan.image.windowsInstallFile);
            const std::string wimPath = fromPath(tmp / "install.wim");
            {
                File out(wimPath, File::Write);
                uint64_t pos = 0;
                Meter m(ctx, T("Preparing install.wim"), plan.image.windowsInstallSize);
                iso.streamFile(iso.entries()[size_t(wi)], [&](const uint8_t* p, size_t n) {
                    out.writeAt(pos, p, n);
                    pos += n;
                    m.update(pos);
                }, &ctx);
            }
            fs::create_directories(tmp / "parts");
            ctx.progress(T("Splitting install.wim"), 0, 0);
            if (runProgram({"wimlib-imagex", "split", wimPath, fromPath(tmp / "parts" / "install.swm"), "3800"}, ctx) != 0)
                throw Error(T("wimlib could not split install.wim"));
            fs::remove(u8(wimPath));
            cp.splitDir = fromPath(tmp / "parts");
        }
        try {
            copyTree(cp, ctx);
        } catch (...) {
            if (!splitTmp.empty())
                fs::remove_all(u8(splitTmp));
            throw;
        }
        if (o.windows.any()) {
            const std::string xml = makeAutounattend(o.windows);
            writer->writeFile("/autounattend.xml", xml.size(), [&](const FileSink& s) {
                s(reinterpret_cast<const uint8_t*>(xml.data()), xml.size());
            });
            ctx.log(T("Windows setup options written (autounattend.xml)"));
        }
    }
    if (writer && o.autorun && iso.find("/autorun.inf") < 0) {
        writeAutorun(*writer, o.label);
        const std::vector<uint8_t> ico(autorunIconData(), autorunIconData() + autorunIconSize());
        writer->writeFile("/autorun.ico", ico.size(), [&](const FileSink& s) { s(ico.data(), ico.size()); });
    }
    if (writer) {
        ctx.log(T("Writing directories and the file allocation table"));
        writer->finish();
    }
    disk->flush();
    if (writer && cp.iso && o.verify)
        verifyTree(cp, ctx);
    if (!splitTmp.empty())
        fs::remove_all(u8(splitTmp));
    if (!mountPoint.empty())
        disk->unmountPartition(mountPoint, ctx);
    // Release the main partition (on Windows this unlocks its volume) before anything else uses it.
    writer.reset();
    mainTarget.reset();

    // ---- dual layout: the second (NTFS) partition gets the whole image
    if (plan.dualPartition) {
        ctx.log(format(T("Formatting %1"), {"NTFS"}));
        ctx.progress(T("Formatting"), 0, 0);
        osFormat(*disk, 2, data.offset, data.size, "ntfs", data.name, 0, ctx);
        const std::string mp = disk->mountPartition(2, data.offset, "ntfs", ctx);
        DirTreeWriter dw(mp, data.size, false);
        CopyPlan all;
        all.iso = &iso;
        all.plan = &plan;
        all.writer = &dw;
        copyTree(all, ctx);
        if (o.windows.any()) {
            const std::string xml = makeAutounattend(o.windows);
            dw.writeFile("/autounattend.xml", xml.size(), [&](const FileSink& s) { s(reinterpret_cast<const uint8_t*>(xml.data()), xml.size()); });
        }
        dw.finish();
        if (o.verify)
            verifyTree(all, ctx);
        disk->unmountPartition(mp, ctx);
    }

    // ---- boot loaders
    if (plan.biosLoader == "bootmgr") {
        ctx.log(T("Installing the Windows BIOS boot record"));
        auto part = disk->partition(1, main.offset, main.size, ctx);
        if (o.fs == FsType::Fat32)
            writeFat32BootmgrRecord(*part);
        else if (o.fs == FsType::Ntfs)
            writeNtfsBootmgrRecord(*part);
    }
#ifndef _WIN32
    if (plan.biosLoader == "syslinux") {
        ctx.log(T("Installing Syslinux for BIOS boot"));
        ctx.progress(T("Installing boot loader"), 0, 0);
        std::string cfgDir = "/syslinux";
        iso.walk([&](const IsoEntry& e) {
            if (!e.dir && (iequals(e.name, "isolinux.cfg") || iequals(e.name, "syslinux.cfg")) && cfgDir == "/syslinux")
                cfgDir = e.path.substr(0, e.path.rfind('/'));
        });
        disk->flush();
        const std::string dev = disk->partitionPath(1, main.offset, ctx);
        if (runProgram({"syslinux", "--install", "--directory", cfgDir.empty() ? "/" : cfgDir, dev}, ctx) != 0)
            throw Error(T("Syslinux could not be installed"));
    } else if (plan.biosLoader == "grub") {
        ctx.log(T("Installing GRUB for BIOS boot"));
        ctx.progress(T("Installing boot loader"), 0, 0);
        disk->flush();
        const std::string mp = disk->mountPartition(1, main.offset, lowerFs(o.fs), ctx);
        std::string whole = drive.path;
        if (drive.isFile) {   // the loop device that carries the image file
            whole = disk->partitionPath(1, main.offset, ctx);
            whole = whole.substr(0, whole.size() - 2);
        }
        const int rc = runProgram({"grub-install", "--target=i386-pc", "--boot-directory=" + mp + "/boot", "--force", "--no-floppy",
                                   "--modules=part_msdos part_gpt fat ntfs exfat iso9660 search configfile normal linux", whole},
                                  ctx);
        disk->unmountPartition(mp, ctx);
        if (rc != 0)
            throw Error(T("GRUB could not be installed"));
    }
    // ---- persistence
    if (persistIndex) {
        const PartSpec& ps = layout.parts[size_t(persistIndex - 1)];
        ctx.log(format(T("Creating the %1 persistence partition (%2)"), {ps.name, humanBytes(ps.size)}));
        ctx.progress(T("Creating persistence"), 0, 0);
        osFormat(*disk, persistIndex, ps.offset, ps.size, "ext4", ps.name, 0, ctx);
        if (plan.image.persistenceKind == "debian") {
            const std::string mp = disk->mountPartition(persistIndex, ps.offset, "ext4", ctx);
            File f(mp + "/persistence.conf", File::Write);
            const std::string conf = "/ union\n";
            f.writeAt(0, conf.data(), conf.size());
            f.close();
            disk->unmountPartition(mp, ctx);
        }
    }
#endif
    // ---- UEFI:NTFS
    if (uefiIndex) {
        ctx.log(T("Adding the UEFI:NTFS boot partition"));
        const PartSpec& u = layout.parts[size_t(uefiIndex - 1)];
        const std::vector<uint8_t> img = uefiNtfsImage();
        auto part = disk->partition(uefiIndex, u.offset, u.size, ctx);
        part->writeAt(0, img.data(), std::min<size_t>(img.size(), size_t(u.size)));
        part->flush();
    }
    disk->flush();
    disk->release(o.eject, ctx);
    ctx.log(T("Done"));
}

BadBlockReport quickFakeCheck(const DriveInfo& selected, const JobContext& ctx)
{
    DriveInfo drive;
    if (!findDrive(selected.path, drive, selected.isFile))
        throw Error(T("The drive was removed"));
    auto disk = Disk::open(drive, true, ctx);
    const uint32_t ss = disk->sectorSize();
    const uint32_t block = std::max<uint32_t>(4096, ss);
    const uint64_t blocks = disk->size() / block;
    if (blocks < 16)
        throw Error(T("The drive is too small"));
    // 512 spots spread over the drive, always including the first and the last block.
    const int spots = int(std::min<uint64_t>(512, blocks));
    std::vector<uint64_t> where(static_cast<size_t>(spots));
    for (int i = 0; i < spots; ++i)
        where[size_t(i)] = (blocks - 1) * uint64_t(i) / uint64_t(spots - 1) * block;
    std::random_device rd;
    const uint64_t nonce = (uint64_t(rd()) << 32) | rd();
    std::vector<std::vector<uint8_t>> saved(static_cast<size_t>(spots));
    std::vector<uint8_t> buf(block);
    auto stamp = [&](uint64_t off) {
        for (size_t k = 0; k < block; k += 8) {
            const uint64_t v = (off + k) ^ nonce;
            std::memcpy(buf.data() + k, &v, 8);
        }
    };
    BadBlockReport r;
    r.testedBytes = disk->size();
    bool restored = false;
    auto restore = [&] {
        if (restored)
            return;
        restored = true;
        for (int i = 0; i < spots; ++i)
            if (!saved[size_t(i)].empty())
                disk->writeAt(where[size_t(i)], saved[size_t(i)].data(), block);
        disk->flush();
    };
    try {
        Meter m(ctx, T("Checking the real capacity"), uint64_t(spots) * 3);
        for (int i = 0; i < spots; ++i) {
            ctx.checkCancel();
            saved[size_t(i)].resize(block);
            disk->readAt(where[size_t(i)], saved[size_t(i)].data(), block);
            m.update(uint64_t(i));
        }
        for (int i = 0; i < spots; ++i) {
            ctx.checkCancel();
            stamp(where[size_t(i)]);
            disk->writeAt(where[size_t(i)], buf.data(), block);
            m.update(uint64_t(spots + i));
        }
        disk->flush();   // also drops the system's cache, so the reads below come from the drive
        uint64_t firstBad = UINT64_MAX;
        std::vector<uint8_t> got(block);
        for (int i = 0; i < spots; ++i) {
            ctx.checkCancel();
            stamp(where[size_t(i)]);
            bool ok = true;
            try {
                disk->readAt(where[size_t(i)], got.data(), block);
                ok = !std::memcmp(got.data(), buf.data(), block);
            } catch (const Error&) {
                ok = false;
            }
            if (!ok) {
                r.badSectors += block / ss;
                firstBad = std::min(firstBad, where[size_t(i)]);
                if (r.badRanges.size() < 64)
                    r.badRanges.push_back({where[size_t(i)], where[size_t(i)] + block});
            }
            m.update(uint64_t(2 * spots + i));
        }
        restore();
        m.update(uint64_t(spots) * 3, true);
        if (firstBad != UINT64_MAX && firstBad > 0) {
            r.fakeCapacity = true;
            r.realCapacity = firstBad;
        }
    } catch (...) {
        try {
            restore();
        } catch (...) {
        }
        throw;
    }
    disk->release(false, ctx);
    return r;
}

BadBlockReport runBadBlocks(const DriveInfo& selected, int passes, const JobContext& ctx)
{
    DriveInfo drive;
    if (!findDrive(selected.path, drive, selected.isFile))
        throw Error(T("The drive was removed"));
    auto disk = Disk::open(drive, true, ctx);
    BadBlockReport r = badBlocksOn(*disk, passes, ctx);
    disk->release(false, ctx);
    return r;
}

// ---------------------------------------------------------------- save drive image

namespace {

void be32w(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (24 - 8 * i)); }
void be64w(uint8_t* p, uint64_t v) { be32w(p, uint32_t(v >> 32)); be32w(p + 4, uint32_t(v)); }

std::vector<uint8_t> vhdFooter(uint64_t size)
{
    std::vector<uint8_t> f(512, 0);
    std::memcpy(f.data(), "conectix", 8);
    be32w(f.data() + 8, 2);
    be32w(f.data() + 12, 0x00010000);
    be64w(f.data() + 16, 0xFFFFFFFFFFFFFFFFULL);
    be32w(f.data() + 24, uint32_t(std::time(nullptr) - 946684800));
    std::memcpy(f.data() + 28, "baz ", 4);
    be32w(f.data() + 32, 0x00010000);
    std::memcpy(f.data() + 36, "Wi2k", 4);
    be64w(f.data() + 40, size);
    be64w(f.data() + 48, size);
    // CHS geometry as the VHD specification computes it.
    uint64_t total = size / 512;
    if (total > 65535ULL * 16 * 255)
        total = 65535ULL * 16 * 255;
    uint32_t spt, heads, cth;
    if (total >= 65535ULL * 16 * 63) {
        spt = 255;
        heads = 16;
        cth = uint32_t(total / spt);
    } else {
        spt = 17;
        cth = uint32_t(total / spt);
        heads = (cth + 1023) / 1024;
        if (heads < 4) heads = 4;
        if (cth >= heads * 1024 || heads > 16) {
            spt = 31;
            heads = 16;
            cth = uint32_t(total / spt);
        }
        if (cth >= heads * 1024) {
            spt = 63;
            heads = 16;
            cth = uint32_t(total / spt);
        }
    }
    const uint32_t cyl = cth / heads;
    f[56] = uint8_t(cyl >> 8);
    f[57] = uint8_t(cyl);
    f[58] = uint8_t(heads);
    f[59] = uint8_t(spt);
    be32w(f.data() + 60, 2);   // fixed disk
    std::random_device rd;
    for (int i = 0; i < 16; ++i)
        f[size_t(68 + i)] = uint8_t(rd());
    uint32_t sum = 0;
    for (uint8_t b : f)
        sum += b;
    be32w(f.data() + 64, ~sum);
    return f;
}

} // namespace

void saveDriveImage(const DriveInfo& selected, const std::string& out, bool usedOnly, const JobContext& ctx)
{
    DriveInfo drive;
    if (!findDrive(selected.path, drive, selected.isFile))
        throw Error(T("The drive was removed"));
    auto disk = Disk::open(drive, false, ctx);
    uint64_t size = disk->size();
    if (usedOnly) {
        uint64_t last = 0;
        try {
            for (const auto& p : readPartitionTable(*disk))
                last = std::max(last, p.offset + p.size);
        } catch (...) {
        }
        if (last > 0 && last < size)
            size = std::min(size, (last + 2 * MiB) / MiB * MiB);   // keep a backup GPT if there is one
    }
    const bool vhd = endsWith(lower(out), ".vhd");
    const Compression comp = vhd ? Compression::None : compressionFromName(out);
    ctx.log(format(T("Saving %1 of %2 to %3"), {humanBytes(size), drive.displayName(), out}));
    auto sink = ImageSink::create(out, comp);
    Meter m(ctx, T("Saving image"), size);
    std::vector<uint8_t> buf(kChunk);
    for (uint64_t off = 0; off < size;) {
        ctx.checkCancel();
        const size_t n = size_t(std::min<uint64_t>(buf.size(), size - off));
        disk->readAt(off, buf.data(), n);
        sink->write(buf.data(), n);
        off += n;
        m.update(off);
    }
    if (vhd) {
        const auto f = vhdFooter(size);
        sink->write(f.data(), f.size());
    }
    sink->finish();
    sink.reset();
    handOverToUser(out);
    m.update(size, true);
    ctx.log(T("Done"));
}

// ---------------------------------------------------------------- hashes

std::vector<std::pair<HashAlgo, std::string>> hashFile(const std::string& path, const std::vector<HashAlgo>& algos, const JobContext& ctx)
{
    File f(path, File::Read);
    const uint64_t size = f.size();
    std::vector<std::unique_ptr<Hasher>> hs;
    for (HashAlgo a : algos)
        hs.push_back(Hasher::create(a));
    Meter m(ctx, T("Calculating checksums"), size);
    // Double buffering: read the next chunk while every algorithm hashes the current one.
    std::vector<uint8_t> a(kChunk), b(kChunk);
    size_t na = f.readAt(0, a.data(), a.size());
    uint64_t off = na;
    while (na) {
        ctx.checkCancel();
        std::vector<std::future<void>> work;
        for (auto& h : hs)
            work.push_back(std::async(std::launch::async, [&h, &a, na] { h->update(a.data(), na); }));
        const size_t nb = f.readAt(off, b.data(), b.size());
        for (auto& w : work)
            w.get();
        off += nb;
        m.update(off - nb);
        std::swap(a, b);
        na = nb;
    }
    m.update(size, true);
    std::vector<std::pair<HashAlgo, std::string>> out;
    for (size_t i = 0; i < algos.size(); ++i)
        out.emplace_back(algos[i], hs[i]->finishHex());
    return out;
}

std::string findPublishedHash(const std::string& imagePath, HashAlgo& algo)
{
    const fs::path img = u8(imagePath);
    const std::string name = fromPath(img.filename());
    const fs::path dir = img.parent_path();
    struct Cand { std::string file; HashAlgo a; };
    const std::vector<Cand> cands = {
        {name + ".sha256", HashAlgo::Sha256}, {name + ".sha256sum", HashAlgo::Sha256}, {name + ".sha512", HashAlgo::Sha512},
        {name + ".sha1", HashAlgo::Sha1},     {name + ".md5", HashAlgo::Md5},          {"SHA256SUMS", HashAlgo::Sha256},
        {"sha256sum.txt", HashAlgo::Sha256},  {"SHA512SUMS", HashAlgo::Sha512},        {"SHA1SUMS", HashAlgo::Sha1},
        {"MD5SUMS", HashAlgo::Md5},           {"CHECKSUM", HashAlgo::Sha256},
    };
    const size_t hexLen[] = {32, 40, 64, 128};
    for (const Cand& c : cands) {
        const std::string text = readTextFile(fromPath(dir / u8(c.file)));
        if (text.empty())
            continue;
        const size_t want = hexLen[int(c.a)];
        for (std::string line : split(text, '\n')) {
            line = trim(line);
            // "hash  name", "hash *name", "SHA256 (name) = hash", or a lone hash
            std::string hash, file;
            if (line.find(") = ") != std::string::npos) {
                const auto l = line.find('('), r = line.find(") = ");
                file = line.substr(l + 1, r - l - 1);
                hash = trim(line.substr(r + 4));
            } else {
                const auto sp = line.find_first_of(" \t");
                hash = line.substr(0, sp);
                file = sp == std::string::npos ? "" : trim(line.substr(sp));
                if (!file.empty() && file[0] == '*')
                    file.erase(0, 1);
            }
            if (hash.size() != want || !std::all_of(hash.begin(), hash.end(), [](char ch) { return std::isxdigit(static_cast<unsigned char>(ch)); }))
                continue;
            if (file.empty() || file == name || endsWith(file, "/" + name)) {
                algo = c.a;
                return lower(hash);
            }
        }
    }
    return {};
}

} // namespace bootaz
