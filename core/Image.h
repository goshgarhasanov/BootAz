#pragma once
// What a selected image is, what it can boot, and how it should be written.

#include "Util.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace bootaz {

enum class ImageKind { Unknown, Iso, RawDisk, Vhd, Compressed };
enum class Compression { None, Gzip, Xz, Zstd, Bzip2 };

struct ImageInfo {
    std::string path;
    std::string fileName;
    ImageKind kind = ImageKind::Unknown;
    Compression compression = Compression::None;
    uint64_t fileSize = 0;
    uint64_t dataSize = 0;          // bytes that DD mode writes (uncompressed; 0 = unknown)
    std::string label;
    std::string isoLabel;           // ISO 9660 label that boot configs may search for
    std::string friendlyName;       // "Windows 11 x64", "Ubuntu 24.04", "Alpine 3.20"
    std::string fsTree;             // "UDF", "ISO 9660 + Rock Ridge", ...

    // Boot capabilities found in the image.
    bool isohybrid = false;         // the image itself starts with an MBR / GPT (can be written as is)
    bool gptInImage = false;
    bool elToritoBios = false;
    bool elToritoEfi = false;
    std::vector<std::string> efiLoaders;   // "/EFI/BOOT/BOOTX64.EFI", ...
    // Secure Boot: how the main x64 UEFI loader is signed.
    bool efiSigned = false;
    std::string efiSigner;                 // "Microsoft Windows Production PCA 2011", "Windows UEFI CA 2023", "Microsoft UEFI CA (shim)" ...
    bool efiOldMicrosoftCa = false;        // only the 2011 Windows CA: blocked on PCs with the 2024+ revocation
    bool hasSyslinux = false;
    bool hasGrub = false;
    std::vector<std::string> bootConfigs;  // syslinux / isolinux / grub config files

    // What kind of system it installs.
    bool isWindows = false;
    std::string windowsInstallFile;        // "/sources/install.wim"
    uint64_t windowsInstallSize = 0;
    bool isWindows11 = false;
    bool isLinux = false;
    std::string distro;                    // "Ubuntu", "Debian", "Fedora", ...
    std::string persistenceKind;           // "casper", "debian" or empty when not supported
    std::string persistenceLabel;          // partition label the live system looks for ("writable", "casper-rw", "persistence")

    uint64_t filesTotal = 0;               // sum of file sizes (ISO mode)
    uint64_t fileCount = 0;
    uint64_t largestFile = 0;
    std::vector<std::string> over4GB;      // files a FAT32 drive cannot hold

    bool canIsoMode() const { return kind == ImageKind::Iso && fileCount > 0; }
    bool canDdMode() const { return kind != ImageKind::Unknown; }
    bool uefiBootable() const { return !efiLoaders.empty() || elToritoEfi || (isohybrid && gptInImage); }
    bool biosBootable() const { return elToritoBios || isohybrid; }
    bool ddRecommended() const;            // what the "write mode" defaults to
};

// Detects the kind of file and, for ISO images, everything above. Throws Error.
ImageInfo analyzeImage(const std::string& path);
// A drive label that names the system: "WIN11 X64" / "UBUNTU_2404" on FAT32, longer on NTFS / exFAT.
std::string friendlyLabel(const ImageInfo& info, size_t maxChars);
const char* compressionName(Compression c);
bool compressionSupported(Compression c);   // built with that decoder?

// Sequential reader that decompresses on the fly (DD mode, hashing, verification).
class ImageStream {
public:
    virtual ~ImageStream() = default;
    virtual size_t read(void* buf, size_t len) = 0;   // 0 at end
    virtual uint64_t sourcePosition() const = 0;      // bytes consumed from the file (for progress)
    static std::unique_ptr<ImageStream> open(const std::string& path, Compression c, uint64_t limit = 0);
};

// Writes a file while compressing it (saving a drive to .img.gz / .img.xz / .img.zst).
class ImageSink {
public:
    virtual ~ImageSink() = default;
    virtual void write(const void* buf, size_t len) = 0;
    virtual void finish() = 0;
    static std::unique_ptr<ImageSink> create(const std::string& path, Compression c);
};

Compression compressionFromName(const std::string& fileName);

} // namespace bootaz
