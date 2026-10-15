#pragma once
// The operations BootAz performs on a drive. Each one runs synchronously, reports through a
// JobContext and throws Error / Cancelled. The CLI calls them directly; the GUI runs the CLI
// (elevated) in JSON mode and shows its progress.

#include "Disk.h"
#include "Hash.h"
#include "Image.h"
#include "Partition.h"
#include "Unattend.h"

#include <cstdint>
#include <string>
#include <vector>

namespace bootaz {

enum class FsType { Fat32, Ntfs, Exfat, Ext2, Ext3, Ext4 };
enum class TargetSystem { Uefi, Bios, BiosUefi };
enum class WriteMode { Auto, Iso, Dd };
// How a Windows image whose install.wim is over 4 GB is laid out.
enum class WinLayout { Auto, Ntfs /* NTFS + UEFI:NTFS */, Split /* FAT32, split .swm */, Dual /* FAT32 boot + NTFS */ };

const char* fsName(FsType f);                       // "FAT32", "NTFS", ...
bool parseFs(const std::string& s, FsType& out);
const char* targetName(TargetSystem t);             // "UEFI", "BIOS", "BIOS+UEFI"
bool parseTarget(const std::string& s, TargetSystem& out);
std::vector<FsType> availableFileSystems();         // what this machine can format

struct WriteOptions {
    std::string image;                  // empty: format only (non-bootable)
    WriteMode mode = WriteMode::Auto;
    Scheme scheme = Scheme::Gpt;
    TargetSystem target = TargetSystem::Uefi;
    bool fsAuto = true;
    FsType fs = FsType::Fat32;
    std::string label;                  // "" = from the image
    uint32_t clusterSize = 0;           // 0 = default
    bool quickFormat = true;            // false = zero the whole partition first
    int badBlockPasses = 0;             // 0..4 destructive test before writing
    bool verify = true;                 // read everything back afterwards
    bool eject = false;
    uint64_t persistenceBytes = 0;      // Ubuntu / Debian live persistence
    bool splitWim = false;              // Windows: keep FAT32 and split install.wim
    WinLayout winLayout = WinLayout::Auto;
    bool autorun = true;                // autorun.inf with the label and BootAz icon
    WindowsOptions windows;
    std::string expectedId;             // drive id when it was selected (safety re-check)
};

struct Plan {
    WriteOptions options;               // with every "auto" resolved
    ImageInfo image;
    bool dd = false;
    bool uefiNtfs = false;              // add the small UEFI:NTFS partition
    bool dualPartition = false;         // Windows: FAT32 boot partition + NTFS data partition
    std::string biosLoader;             // "", "bootmgr", "syslinux", "grub", "image"
    std::vector<std::string> warnings;  // shown to the user before starting
    std::vector<std::string> errors;    // the job cannot run
};

// Resolves the options for this drive and image and lists problems. Does not touch the drive.
Plan planWrite(const DriveInfo& drive, const WriteOptions& opt);
void runWrite(const DriveInfo& drive, const Plan& plan, const JobContext& ctx);

struct BadBlockReport {
    uint64_t testedBytes = 0;
    uint64_t badSectors = 0;
    std::vector<std::pair<uint64_t, uint64_t>> badRanges;   // byte ranges (first 64)
    bool fakeCapacity = false;
    uint64_t realCapacity = 0;          // when fake: where the drive stops storing data
};
BadBlockReport runBadBlocks(const DriveInfo& drive, int passes, const JobContext& ctx);

// Fast, non-destructive check for fake capacity: writes stamps at a few hundred spots spread over
// the drive, reads them back and puts the original data back (like ValiDrive).
BadBlockReport quickFakeCheck(const DriveInfo& drive, const JobContext& ctx);

// Reads a drive into an image file (.img, .img.gz, .img.xz, .img.zst or a fixed .vhd).
// usedOnly stops after the last partition instead of copying the whole drive.
void saveDriveImage(const DriveInfo& drive, const std::string& out, bool usedOnly, const JobContext& ctx);

std::vector<std::pair<HashAlgo, std::string>> hashFile(const std::string& path, const std::vector<HashAlgo>& algos,
                                                       const JobContext& ctx);
// Looks for "<image>.sha256", SHA256SUMS etc. next to the image; returns the expected digest or "".
std::string findPublishedHash(const std::string& imagePath, HashAlgo& algo);

} // namespace bootaz
