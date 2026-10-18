#pragma once
// Drives: listing, opening for raw access, unmounting / locking, and partition access.
// Implemented in Disk_linux.cpp and Disk_win.cpp.

#include "Util.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace bootaz {

struct VolumeInfo {
    std::string path;          // "/dev/sdb1" or "\\.\E:"
    std::string mountPoint;    // "/media/user/USB" or "E:\"
    std::string label;
    std::string fsType;
    uint64_t offset = 0;
    uint64_t size = 0;
};

struct DriveInfo {
    std::string path;          // "/dev/sdb" or "\\.\PhysicalDrive2" (or an image file in tests)
    std::string id;            // stable id used to re-check the drive before writing (serial or path)
    std::string vendor;
    std::string model;
    std::string serial;
    std::string bus;           // "USB", "SD", "SATA", "NVMe", "Virtual", ...
    uint64_t size = 0;
    uint32_t sectorSize = 512;
    bool removable = false;
    bool usb = false;
    bool system = false;       // holds the running operating system: never writable
    bool readOnly = false;
    bool isFile = false;       // a disk image file used as target
    std::vector<VolumeInfo> volumes;

    std::string displayName() const;   // "SanDisk Ultra (E:) [14.9 GB]"
};

struct ListOptions {
    bool includeFixed = false;     // internal / USB hard disks too (Rufus: "List USB Hard Drives")
    bool includeVirtual = false;   // loop devices etc. (testing)
};

std::vector<DriveInfo> listDrives(const ListOptions& opt = {});
// Looks up one drive by path (also accepts image files when allowFile is set).
bool findDrive(const std::string& path, DriveInfo& out, bool allowFile = false);
// True when this process may write raw devices (root / Administrator).
bool haveAdminRights();

// A byte-addressable area: a whole drive, one partition, or a file.
class BlockTarget {
public:
    virtual ~BlockTarget() = default;
    virtual void readAt(uint64_t offset, void* buf, size_t len) = 0;
    virtual void writeAt(uint64_t offset, const void* buf, size_t len) = 0;
    virtual uint64_t size() const = 0;
    virtual uint32_t sectorSize() const = 0;
    virtual void flush() = 0;
};

// Exclusive raw access to a drive for the duration of a job. Opening it unmounts (Linux) or
// locks and dismounts (Windows) every volume on the drive; the destructor releases it.
class Disk : public BlockTarget {
public:
    static std::unique_ptr<Disk> open(const DriveInfo& drive, bool writable, const JobContext& ctx);
    ~Disk() override;

    void readAt(uint64_t offset, void* buf, size_t len) override;
    void writeAt(uint64_t offset, const void* buf, size_t len) override;
    uint64_t size() const override { return size_; }
    uint32_t sectorSize() const override { return sector_; }
    void flush() override;

    // After writing a new partition table: make the OS read it and wait for the partitions.
    void rescanPartitions(int expected, const JobContext& ctx);
    // Device / volume path of partition `index` (1-based) for OS formatting tools.
    std::string partitionPath(int index, uint64_t offset, const JobContext& ctx);
    // A target confined to one partition (writes go through the volume where the OS requires it).
    std::unique_ptr<BlockTarget> partition(int index, uint64_t offset, uint64_t size, const JobContext& ctx);
    // Mounts partition `index` read/write at a temporary folder and returns that folder.
    std::string mountPartition(int index, uint64_t offset, const std::string& fsType, const JobContext& ctx);
    void unmountPartition(const std::string& mountPoint, const JobContext& ctx);
    // Lets the OS mount the drive again (end of job) and optionally ejects it.
    void release(bool eject, const JobContext& ctx);

    const DriveInfo& info() const { return drive_; }

private:
    Disk() = default;
    void rawRead(uint64_t offset, void* buf, size_t len);
    void rawWrite(uint64_t offset, const void* buf, size_t len);

    DriveInfo drive_;
    uint64_t size_ = 0;
    uint32_t sector_ = 512;
    bool writable_ = false;
#ifdef _WIN32
    void* h_ = nullptr;
    std::vector<void*> locks_;    // locked volume handles
#else
    int fd_ = -1;
    std::string loopFor_;          // image file target: the loop device we attached
#endif
    std::vector<std::string> mounts_;
};

// A window into another target (a partition inside a disk).
class SubTarget : public BlockTarget {
public:
    SubTarget(BlockTarget& parent, uint64_t offset, uint64_t size) : p_(parent), off_(offset), size_(size) {}
    void readAt(uint64_t o, void* b, size_t n) override;
    void writeAt(uint64_t o, const void* b, size_t n) override;
    uint64_t size() const override { return size_; }
    uint32_t sectorSize() const override { return p_.sectorSize(); }
    void flush() override { p_.flush(); }

private:
    BlockTarget& p_;
    uint64_t off_, size_;
};

// Formats partition `index` with the operating system's own formatter (NTFS, exFAT, ext2/3/4).
// `fs` is lower case ("ntfs", "exfat", "ext4"); cluster 0 = default.
void osFormat(Disk& disk, int index, uint64_t offset, uint64_t size, const std::string& fs, const std::string& label,
              uint32_t cluster, const JobContext& ctx);
// File systems the operating system can create here (beside our own FAT32).
bool osCanFormat(const std::string& fs);

// Files created as root (pkexec / sudo) are handed to the user who started BootAz. No-op on Windows.
void handOverToUser(const std::string& path);
// A new, empty folder only this user can use (mkdtemp), on a file system with at least
// `needBytes` free (Linux tries $TMPDIR, /var/tmp, then /tmp, which is often in RAM); throws Error.
std::string makePrivateTempDir(const std::string& prefix, uint64_t needBytes = 0);
// When running with elevated rights on behalf of a user: may that user write `path`? (Image files
// as targets and saved images must not let the elevated job overwrite system files.)
bool invokingUserMayWrite(const std::string& path, std::string* why = nullptr);

// Runs an external program (mkfs, mount, ...). Returns the exit code; output goes to the log.
int runProgram(const std::vector<std::string>& argv, const JobContext& ctx, std::string* output = nullptr);
// Finds a program in PATH (and the usual sbin folders); empty if missing.
std::string findProgram(const std::string& name);

} // namespace bootaz
