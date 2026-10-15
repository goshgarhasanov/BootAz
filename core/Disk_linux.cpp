// Linux drive backend: /sys/block for the list, the udev database for names and serials,
// O_EXCL raw access, umount / swapoff, BLKRRPART, mkfs / mount through the system tools.

#include "Disk.h"
#include "I18n.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <thread>

#include <fcntl.h>
#include <linux/fs.h>
#include <poll.h>
#include <pwd.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/swap.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace bootaz {

namespace {

std::string readSys(const std::string& path) { return trim(readTextFile(path)); }

// /run/udev/data/b<major>:<minor> holds "E:KEY=value" lines.
std::map<std::string, std::string> udevProps(const std::string& sysBlock)
{
    std::map<std::string, std::string> m;
    const std::string dev = readSys(sysBlock + "/dev");
    if (dev.empty())
        return m;
    std::istringstream in(readTextFile("/run/udev/data/b" + dev));
    std::string line;
    while (std::getline(in, line)) {
        if (startsWith(line, "E:")) {
            const auto eq = line.find('=');
            if (eq != std::string::npos)
                m[line.substr(2, eq - 2)] = line.substr(eq + 1);
        }
    }
    return m;
}

// Decodes udev's \x20 escapes.
std::string udevUnescape(const std::string& s)
{
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 3 < s.size() && s[i + 1] == 'x') {
            o += char(std::stoi(s.substr(i + 2, 2), nullptr, 16));
            i += 3;
        } else {
            o += s[i];
        }
    }
    return o;
}

struct MountEntry { std::string dev, dir, type; };

std::string unescapeMount(const std::string& s)
{
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 3 < s.size()) {
            o += char(std::stoi(s.substr(i + 1, 3), nullptr, 8));
            i += 3;
        } else {
            o += s[i];
        }
    }
    return o;
}

std::vector<MountEntry> mounts()
{
    std::vector<MountEntry> v;
    std::istringstream in(readTextFile("/proc/self/mounts"));
    std::string dev, dir, type, rest;
    while (in >> dev >> dir >> type) {
        std::getline(in, rest);
        v.push_back({dev, unescapeMount(dir), type});
    }
    return v;
}

// Kernel name of a block device node ("/dev/sdb1" -> "sdb1"), following symlinks.
std::string kernelName(const std::string& dev)
{
    std::error_code ec;
    const fs::path real = fs::canonical(dev, ec);
    if (ec)
        return {};
    struct stat st {};
    if (::stat(real.c_str(), &st) != 0 || !S_ISBLK(st.st_mode))
        return {};
    const std::string link = "/sys/dev/block/" + std::to_string(major(st.st_rdev)) + ":" + std::to_string(minor(st.st_rdev));
    const fs::path p = fs::canonical(link, ec);
    return ec ? std::string() : p.filename().string();
}

// Whole-disk names that hold `kname` (itself, its parent disk, or the disks under a dm/md device).
void disksOf(const std::string& kname, std::set<std::string>& out, int depth = 0)
{
    if (kname.empty() || depth > 8)
        return;
    if (fs::exists("/sys/block/" + kname)) {
        const fs::path slaves = "/sys/block/" + kname + "/slaves";
        bool any = false;
        std::error_code ec;
        if (fs::exists(slaves))
            for (const auto& s : fs::directory_iterator(slaves, ec)) {
                disksOf(s.path().filename().string(), out, depth + 1);
                any = true;
            }
        if (!any)
            out.insert(kname);
        return;
    }
    // A partition: its sysfs folder sits inside the parent disk's folder.
    std::error_code ec;
    const fs::path p = fs::canonical("/sys/class/block/" + kname, ec);
    if (!ec) {
        const std::string parent = p.parent_path().filename().string();
        if (fs::exists("/sys/block/" + parent))
            disksOf(parent, out, depth + 1);
        const fs::path slaves = p / "slaves";
        if (fs::exists(slaves))
            for (const auto& s : fs::directory_iterator(slaves, ec))
                disksOf(s.path().filename().string(), out, depth + 1);
    }
}

std::set<std::string> systemDisks()
{
    std::set<std::string> sys;
    static const std::set<std::string> critical = {"/", "/boot", "/boot/efi", "/efi", "/usr", "/var", "/home"};
    for (const MountEntry& m : mounts())
        if (critical.count(m.dir) && startsWith(m.dev, "/dev/"))
            disksOf(kernelName(m.dev), sys);
    std::istringstream sw(readTextFile("/proc/swaps"));
    std::string line;
    std::getline(sw, line);
    while (std::getline(sw, line)) {
        const std::string dev = line.substr(0, line.find_first_of(" \t"));
        if (startsWith(dev, "/dev/"))
            disksOf(kernelName(dev), sys);
    }
    return sys;
}

std::string partName(const std::string& disk, int n)
{
    return disk + (std::isdigit(static_cast<unsigned char>(disk.back())) ? "p" : "") + std::to_string(n);
}

DriveInfo describe(const std::string& name, const std::set<std::string>& sys, const std::vector<MountEntry>& mnt)
{
    const std::string sb = "/sys/block/" + name;
    DriveInfo d;
    d.path = "/dev/" + name;
    d.size = std::stoull("0" + readSys(sb + "/size")) * 512;
    const std::string lbs = readSys(sb + "/queue/logical_block_size");
    d.sectorSize = lbs.empty() ? 512 : uint32_t(std::stoul(lbs));
    d.removable = readSys(sb + "/removable") == "1";
    d.readOnly = readSys(sb + "/ro") == "1";
    d.system = sys.count(name) > 0;

    std::error_code ec;
    const std::string devpath = fs::canonical(sb, ec).string();
    const auto u = udevProps(sb);
    auto prop = [&](const char* k) {
        const auto it = u.find(k);
        return it == u.end() ? std::string() : it->second;
    };
    d.vendor = trim(udevUnescape(prop("ID_VENDOR_ENC")));
    if (d.vendor.empty())
        d.vendor = trim(readSys(sb + "/device/vendor"));
    d.model = trim(udevUnescape(prop("ID_MODEL_ENC")));
    if (d.model.empty())
        d.model = trim(readSys(sb + "/device/model"));
    d.serial = prop("ID_SERIAL_SHORT");
    const std::string bus = prop("ID_BUS");
    d.usb = bus == "usb" || devpath.find("/usb") != std::string::npos;
    if (d.usb)
        d.bus = "USB";
    else if (startsWith(name, "nvme"))
        d.bus = "NVMe";
    else if (startsWith(name, "mmcblk"))
        d.bus = "SD";
    else if (startsWith(name, "loop"))
        d.bus = "Virtual";
    else if (bus == "ata")
        d.bus = "SATA";
    else
        d.bus = bus.empty() ? "?" : upper(bus);
    if (startsWith(name, "mmcblk"))
        d.removable = true;   // SD cards are removable even though the slot says otherwise
    if (startsWith(name, "loop")) {
        d.model = "Loop device";
        const std::string backing = readSys(sb + "/loop/backing_file");
        if (!backing.empty())
            d.model += " (" + fs::path(backing).filename().string() + ")";
    }
    d.id = d.serial.empty() ? d.path + ":" + std::to_string(d.size) : d.serial;

    // Partitions and where they are mounted.
    for (const auto& e : fs::directory_iterator(sb, ec)) {
        const std::string pn = e.path().filename().string();
        if (!startsWith(pn, name) || !fs::exists(e.path() / "partition"))
            continue;
        VolumeInfo v;
        v.path = "/dev/" + pn;
        v.offset = std::stoull("0" + readSys((e.path() / "start").string())) * 512;
        v.size = std::stoull("0" + readSys((e.path() / "size").string())) * 512;
        const auto pu = udevProps(e.path().string());
        if (auto it = pu.find("ID_FS_LABEL_ENC"); it != pu.end())
            v.label = udevUnescape(it->second);
        if (auto it = pu.find("ID_FS_TYPE"); it != pu.end())
            v.fsType = it->second;
        for (const MountEntry& m : mnt)
            if (kernelName(m.dev) == pn) {
                v.mountPoint = m.dir;
                break;
            }
        d.volumes.push_back(v);
    }
    std::sort(d.volumes.begin(), d.volumes.end(), [](const VolumeInfo& a, const VolumeInfo& b) { return a.offset < b.offset; });
    return d;
}

} // namespace

std::vector<DriveInfo> listDrives(const ListOptions& opt)
{
    std::vector<DriveInfo> out;
    const auto sys = systemDisks();
    const auto mnt = mounts();
    std::error_code ec;
    for (const auto& e : fs::directory_iterator("/sys/block", ec)) {
        const std::string name = e.path().filename().string();
        const bool virt = startsWith(name, "loop") || startsWith(name, "nbd");
        if (startsWith(name, "ram") || startsWith(name, "zram") || startsWith(name, "dm-") || startsWith(name, "md") ||
            startsWith(name, "sr") || startsWith(name, "fd") || (virt && !opt.includeVirtual))
            continue;
        if (readSys(e.path().string() + "/size") == "0")
            continue;
        DriveInfo d = describe(name, sys, mnt);
        if (virt && startsWith(name, "loop") && readSys(e.path().string() + "/loop/backing_file").empty())
            continue;
        const bool portable = d.usb || d.removable || (virt && opt.includeVirtual);
        if (!portable && !opt.includeFixed)
            continue;
        out.push_back(d);
    }
    std::sort(out.begin(), out.end(), [](const DriveInfo& a, const DriveInfo& b) { return a.path < b.path; });
    return out;
}

bool findDrive(const std::string& path, DriveInfo& out, bool allowFile)
{
    struct stat st {};
    if (::stat(path.c_str(), &st) != 0)
        return false;
    if (S_ISREG(st.st_mode)) {
        if (!allowFile)
            return false;
        out = DriveInfo{};
        out.path = path;
        out.id = path;
        out.model = fs::path(path).filename().string();
        out.bus = "File";
        out.size = uint64_t(st.st_size);
        out.isFile = true;
        return true;
    }
    const std::string kn = kernelName(path);
    if (kn.empty() || !fs::exists("/sys/block/" + kn))
        return false;   // not a whole disk
    out = describe(kn, systemDisks(), mounts());
    return true;
}

bool haveAdminRights() { return ::geteuid() == 0; }

// ---------------------------------------------------------------- programs

std::string findProgram(const std::string& name)
{
    if (name.find('/') != std::string::npos)
        return ::access(name.c_str(), X_OK) == 0 ? name : std::string();
    std::string path = std::getenv("PATH") ? std::getenv("PATH") : "";
    path += ":/usr/sbin:/sbin:/usr/local/sbin:/usr/bin:/bin";
    for (const std::string& dir : split(path, ':')) {
        if (dir.empty())
            continue;
        const std::string p = dir + "/" + name;
        if (::access(p.c_str(), X_OK) == 0)
            return p;
    }
    return {};
}

int runProgram(const std::vector<std::string>& argv, const JobContext& ctx, std::string* output)
{
    if (argv.empty())
        return -1;
    const std::string exe = findProgram(argv[0]);
    if (exe.empty())
        throw Error(format(T("Required program not found: %1"), {argv[0]}));
    std::string cmd;
    for (const auto& a : argv)
        cmd += (cmd.empty() ? "" : " ") + a;
    ctx.log("$ " + cmd);

    int pipefd[2];
    if (::pipe2(pipefd, O_CLOEXEC) != 0)
        throw Error("pipe() failed");
    const pid_t pid = ::fork();
    if (pid == 0) {
        ::dup2(pipefd[1], 1);
        ::dup2(pipefd[1], 2);
        ::setenv("LC_ALL", "C", 1);
        std::vector<char*> args;
        for (const auto& a : argv)
            args.push_back(const_cast<char*>(a.c_str()));
        args.push_back(nullptr);
        ::execv(exe.c_str(), args.data());
        ::_exit(127);
    }
    ::close(pipefd[1]);
    std::string out, line;
    char buf[4096];
    for (;;) {
        const ssize_t n = ::read(pipefd[0], buf, sizeof buf);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        out.append(buf, size_t(n));
    }
    ::close(pipefd[0]);
    int status = 0;
    ::waitpid(pid, &status, 0);
    for (const std::string& l : split(out, '\n'))
        if (!trim(l).empty())
            ctx.log("  " + trim(l));
    if (output)
        *output = out;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

void handOverToUser(const std::string& path)
{
    if (::geteuid() != 0)
        return;
    const char* uid = std::getenv("PKEXEC_UID");
    if (!uid || !*uid)
        uid = std::getenv("SUDO_UID");
    if (!uid || !*uid)
        return;
    const uid_t u = uid_t(std::strtoul(uid, nullptr, 10));
    const char* gidEnv = std::getenv("SUDO_GID");
    gid_t g = gidEnv && *gidEnv ? gid_t(std::strtoul(gidEnv, nullptr, 10)) : gid_t(-1);
    if (g == gid_t(-1))
        if (const struct passwd* pw = ::getpwuid(u))
            g = pw->pw_gid;
    struct stat st {};
    if (::lstat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != 0)
        return;   // only files this job created as root
    if (::lchown(path.c_str(), u, g) != 0) {
    }
}

std::string makePrivateTempDir(const std::string& prefix)
{
    std::string tmpl = std::string(std::getenv("TMPDIR") && *std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") + "/" + prefix + "-XXXXXX";
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back(0);
    if (!::mkdtemp(buf.data()))
        throw Error(T("Cannot create a temporary folder"));
    return buf.data();
}

bool invokingUserMayWrite(const std::string& path, std::string* why)
{
    if (::geteuid() != 0)
        return true;   // the kernel checks an ordinary user's rights itself
    // Only jobs started from the desktop app through pkexec act on a user's behalf; an administrator
    // who runs the CLI with sudo means what they type.
    const char* uidEnv = std::getenv("PKEXEC_UID");
    if (!uidEnv || !*uidEnv)
        return true;
    const uid_t uid = uid_t(std::strtoul(uidEnv, nullptr, 10));
    struct stat st {};
    if (::lstat(path.c_str(), &st) == 0) {
        if (S_ISLNK(st.st_mode) || !S_ISREG(st.st_mode) || st.st_uid != uid) {
            if (why)
                *why = format(T("%1 is not your own file; choose a file in your own folders"), {path});
            return false;
        }
        return true;
    }
    // A new file: the folder must belong to the user.
    std::string dir = path.substr(0, path.find_last_of('/') == std::string::npos ? 0 : path.find_last_of('/'));
    if (dir.empty())
        dir = ".";
    if (::stat(dir.c_str(), &st) != 0 || st.st_uid != uid) {
        if (why)
            *why = format(T("%1 is not your own folder; choose a folder in your home"), {dir});
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- formatting

bool osCanFormat(const std::string& fsType)
{
    if (fsType == "fat32")
        return true;
    if (fsType == "ntfs")
        return !findProgram("mkntfs").empty() || !findProgram("mkfs.ntfs").empty();
    if (fsType == "exfat")
        return !findProgram("mkfs.exfat").empty();
    if (fsType == "ext2" || fsType == "ext3" || fsType == "ext4")
        return !findProgram("mkfs." + fsType).empty();
    return false;
}

void osFormat(Disk& disk, int index, uint64_t offset, uint64_t size, const std::string& fsType, const std::string& label,
              uint32_t cluster, const JobContext& ctx)
{
    (void)size;
    const std::string dev = disk.partitionPath(index, offset, ctx);
    std::vector<std::string> cmd;
    if (fsType == "ntfs") {
        const std::string prog = findProgram("mkntfs").empty() ? "mkfs.ntfs" : "mkntfs";
        cmd = {prog, "--fast", "--force", "--label", label, "--partition-start", std::to_string(offset / disk.sectorSize()),
               "--heads", "255", "--sectors-per-track", "63"};
        if (cluster)
            cmd.insert(cmd.end(), {"--cluster-size", std::to_string(cluster)});
    } else if (fsType == "exfat") {
        cmd = {"mkfs.exfat", "-L", label};
        if (cluster)
            cmd.insert(cmd.end(), {"-c", std::to_string(cluster)});
    } else if (fsType == "ext2" || fsType == "ext3" || fsType == "ext4") {
        cmd = {"mkfs." + fsType, "-F", "-q", "-L", label};
        if (cluster)
            cmd.insert(cmd.end(), {"-b", std::to_string(cluster)});
    } else {
        throw Error(format(T("Unsupported file system: %1"), {fsType}));
    }
    cmd.push_back(dev);
    if (runProgram(cmd, ctx) != 0)
        throw Error(format(T("Formatting the drive as %1 failed"), {fsType}));
}

// ---------------------------------------------------------------- Disk

namespace {

void unmountAll(const DriveInfo& d, const JobContext& ctx)
{
    std::set<std::string> parts;
    for (const VolumeInfo& v : d.volumes)
        parts.insert(fs::path(v.path).filename().string());
    parts.insert(fs::path(d.path).filename().string());

    // Swap first, then mounts, deepest mount points first.
    std::istringstream sw(readTextFile("/proc/swaps"));
    std::string line;
    std::getline(sw, line);
    while (std::getline(sw, line)) {
        const std::string dev = line.substr(0, line.find_first_of(" \t"));
        if (parts.count(kernelName(dev))) {
            ctx.log(format(T("Turning off swap on %1"), {dev}));
            ::swapoff(dev.c_str());
        }
    }
    std::vector<MountEntry> mine;
    for (const MountEntry& m : mounts())
        if (startsWith(m.dev, "/dev/") && parts.count(kernelName(m.dev)))
            mine.push_back(m);
    std::sort(mine.begin(), mine.end(), [](const MountEntry& a, const MountEntry& b) { return a.dir.size() > b.dir.size(); });
    for (const MountEntry& m : mine) {
        ctx.log(format(T("Unmounting %1 from %2"), {m.dev, m.dir}));
        int tries = 0;
        while (::umount2(m.dir.c_str(), 0) != 0) {
            if (errno != EBUSY || ++tries >= 10)
                throw Error(format(T("Cannot unmount %1 (%2). Close any program or window that uses the drive and try again."),
                                   {m.dir, std::strerror(errno)}));
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
        }
    }
}

} // namespace

std::unique_ptr<Disk> Disk::open(const DriveInfo& drive, bool writable, const JobContext& ctx)
{
    std::unique_ptr<Disk> d(new Disk);
    d->drive_ = drive;
    d->writable_ = writable;
    if (writable && drive.system)
        throw Error(T("This drive holds the running operating system and cannot be written"));
    if (writable && drive.readOnly)
        throw Error(T("The drive is write protected"));

    if (drive.isFile) {
        d->fd_ = ::open(drive.path.c_str(), (writable ? O_RDWR : O_RDONLY) | O_CLOEXEC);
        if (d->fd_ < 0)
            throw Error(format(T("Cannot open %1: %2"), {drive.path, std::strerror(errno)}));
        struct stat st {};
        ::fstat(d->fd_, &st);
        d->size_ = uint64_t(st.st_size);
        d->sector_ = 512;
        return d;
    }
    if (writable)
        unmountAll(drive, ctx);
    const int flags = (writable ? (O_RDWR | O_EXCL) : O_RDONLY) | O_CLOEXEC;
    for (int tries = 0;; ++tries) {
        d->fd_ = ::open(drive.path.c_str(), flags);
        if (d->fd_ >= 0)
            break;
        if (errno == EBUSY && tries < 20) {   // udev / the desktop may still hold it briefly
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            continue;
        }
        if (errno == EACCES || errno == EPERM)
            throw Error(T("Administrator rights are required to write to drives"));
        throw Error(format(T("Cannot open %1: %2"), {drive.path, std::strerror(errno)}));
    }
    uint64_t bytes = 0;
    if (::ioctl(d->fd_, BLKGETSIZE64, &bytes) == 0)
        d->size_ = bytes;
    int ss = 512;
    if (::ioctl(d->fd_, BLKSSZGET, &ss) == 0 && ss > 0)
        d->sector_ = uint32_t(ss);
    if (drive.size && d->size_ != drive.size)
        throw Error(T("The drive changed since it was selected; refresh the list and try again"));
    return d;
}

Disk::~Disk()
{
    JobContext quiet;
    for (const std::string& m : mounts_)
        ::umount2(m.c_str(), MNT_DETACH);
    if (fd_ >= 0)
        ::close(fd_);
    if (!loopFor_.empty()) {
        const std::string loop = loopFor_;
        loopFor_.clear();
        try {
            runProgram({"losetup", "-d", loop}, quiet);
        } catch (...) {
        }
    }
}

void Disk::rawRead(uint64_t offset, void* buf, size_t len)
{
    size_t got = 0;
    while (got < len) {
        const ssize_t n = ::pread(fd_, static_cast<char*>(buf) + got, len - got, off_t(offset + got));
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            throw Error(format(T("Read error at %1 on %2: %3"),
                               {humanBytes(offset + got), drive_.path, n == 0 ? "end of device" : std::strerror(errno)}));
        got += size_t(n);
    }
}

void Disk::rawWrite(uint64_t offset, const void* buf, size_t len)
{
    size_t put = 0;
    while (put < len) {
        const ssize_t n = ::pwrite(fd_, static_cast<const char*>(buf) + put, len - put, off_t(offset + put));
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            if (errno == ENOSPC || n == 0)
                throw Error(T("The drive is full: the image is larger than the drive"));
            throw Error(format(T("Write error at %1 on %2: %3"), {humanBytes(offset + put), drive_.path, std::strerror(errno)}));
        }
        put += size_t(n);
    }
}

void Disk::readAt(uint64_t offset, void* buf, size_t len) { rawRead(offset, buf, len); }

void Disk::writeAt(uint64_t offset, const void* buf, size_t len)
{
    if (!writable_)
        throw Error("Disk opened read-only");
    if (offset + len > size_ && !drive_.isFile)
        throw Error(T("The drive is full: the image is larger than the drive"));
    rawWrite(offset, buf, len);
}

void Disk::flush()
{
    if (fd_ < 0)
        return;
    ::fsync(fd_);
    if (!drive_.isFile)
        ::ioctl(fd_, BLKFLSBUF, 0);
}

void Disk::rescanPartitions(int expected, const JobContext& ctx)
{
    flush();
    if (drive_.isFile)
        return;
    ctx.log(T("Asking the system to read the new partition table"));
    for (int i = 0; i < 20; ++i) {
        if (::ioctl(fd_, BLKRRPART, 0) == 0)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    if (!findProgram("udevadm").empty())
        runProgram({"udevadm", "settle", "--timeout=10"}, ctx);
    // The exclusive handle kept the desktop from mounting the drive while it was wiped; formatters
    // and mount need to open the new partitions, which the kernel refuses while it is held.
    const int shared = ::open(drive_.path.c_str(), O_RDWR | O_CLOEXEC);
    if (shared >= 0) {
        ::close(fd_);
        fd_ = shared;
    }
    for (int n = 1; n <= expected; ++n) {
        const std::string p = "/dev/" + partName(fs::path(drive_.path).filename().string(), n);
        for (int i = 0; i < 40 && !fs::exists(p); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    // A desktop automounter may grab the new partitions right away; let go of them again.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    DriveInfo fresh;
    if (findDrive(drive_.path, fresh))
        for (const VolumeInfo& v : fresh.volumes)
            if (!v.mountPoint.empty()) {
                ctx.log(format(T("Unmounting %1 from %2"), {v.path, v.mountPoint}));
                ::umount2(v.mountPoint.c_str(), MNT_DETACH);
            }
}

std::string Disk::partitionPath(int index, uint64_t offset, const JobContext& ctx)
{
    (void)offset;
    if (drive_.isFile) {
        // Image file target: attach it as a loop device with partitions so mkfs can see them.
        if (loopFor_.empty()) {
            flush();
            std::string out;
            if (runProgram({"losetup", "--find", "--show", "--partscan", drive_.path}, ctx, &out) != 0)
                throw Error(T("Cannot attach the image file as a loop device"));
            loopFor_ = trim(out);
            if (!findProgram("udevadm").empty())
                runProgram({"udevadm", "settle", "--timeout=10"}, ctx);
        }
        const std::string p = partName(loopFor_, index);
        for (int i = 0; i < 40 && !fs::exists(p); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        return p;
    }
    return "/dev/" + partName(fs::path(drive_.path).filename().string(), index);
}

std::unique_ptr<BlockTarget> Disk::partition(int index, uint64_t offset, uint64_t size, const JobContext& ctx)
{
    (void)index;
    (void)ctx;
    // Linux lets the whole-disk handle write anywhere; no volume locking needed.
    return std::make_unique<SubTarget>(*this, offset, size);
}

std::string Disk::mountPartition(int index, uint64_t offset, const std::string& fsType, const JobContext& ctx)
{
    const std::string dev = partitionPath(index, offset, ctx);
    char tmpl[] = "/tmp/bootaz-mnt-XXXXXX";
    if (!::mkdtemp(tmpl))
        throw Error(T("Cannot create a temporary folder"));
    const std::string dir = tmpl;
    std::vector<std::vector<std::string>> attempts;
    if (fsType == "ntfs") {
        attempts.push_back({"mount", "-t", "ntfs3", dev, dir});
        attempts.push_back({"mount", "-t", "ntfs-3g", "-o", "big_writes", dev, dir});
    } else if (fsType == "exfat") {
        attempts.push_back({"mount", "-t", "exfat", dev, dir});
    } else if (fsType == "fat32" || fsType == "vfat") {
        attempts.push_back({"mount", "-t", "vfat", "-o", "utf8,shortname=mixed", dev, dir});
    } else {
        attempts.push_back({"mount", "-t", fsType, dev, dir});
    }
    for (const auto& a : attempts) {
        if (runProgram(a, ctx) == 0) {
            mounts_.push_back(dir);
            return dir;
        }
    }
    ::rmdir(dir.c_str());
    throw Error(format(T("Cannot mount the new %1 partition"), {fsType}));
}

void Disk::unmountPartition(const std::string& mountPoint, const JobContext& ctx)
{
    ::sync();
    if (::umount2(mountPoint.c_str(), 0) != 0) {
        runProgram({"umount", mountPoint}, ctx);
    }
    ::rmdir(mountPoint.c_str());
    mounts_.erase(std::remove(mounts_.begin(), mounts_.end(), mountPoint), mounts_.end());
}

void Disk::release(bool eject, const JobContext& ctx)
{
    for (const std::string& m : std::vector<std::string>(mounts_))
        unmountPartition(m, ctx);
    flush();
    if (fd_ >= 0 && !drive_.isFile)
        ::ioctl(fd_, BLKRRPART, 0);
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    if (!loopFor_.empty()) {
        runProgram({"losetup", "-d", loopFor_}, ctx);
        loopFor_.clear();
    }
    if (eject && !drive_.isFile) {
        if (!findProgram("udisksctl").empty() && runProgram({"udisksctl", "power-off", "-b", drive_.path}, ctx) == 0)
            ctx.log(T("The drive was ejected and can be removed safely"));
        else if (!findProgram("eject").empty())
            runProgram({"eject", drive_.path}, ctx);
    }
}

} // namespace bootaz
