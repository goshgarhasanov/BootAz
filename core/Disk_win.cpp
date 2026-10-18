// Windows drive backend: physical drives through DeviceIoControl, volume locking and
// dismounting, sector-aligned raw I/O, FormatEx (fmifs.dll) for NTFS / exFAT, drive letters,
// and "safely remove" through the configuration manager.

#include "Disk.h"
#include "I18n.h"

#include <windows.h>
#include <cfgmgr32.h>
#include <setupapi.h>
#include <shlobj.h>
#include <winioctl.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <thread>

namespace fs = std::filesystem;

namespace bootaz {

namespace {

std::wstring W(const std::string& s)
{
    const std::u16string u = utf8ToUtf16(s);
    return std::wstring(u.begin(), u.end());
}

std::string U(const wchar_t* s)
{
    std::u16string u;
    for (; s && *s; ++s)
        u += char16_t(*s);
    return utf16ToUtf8(u);
}

std::string winError(DWORD e = GetLastError())
{
    wchar_t* msg = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, e, 0,
                   reinterpret_cast<wchar_t*>(&msg), 0, nullptr);
    std::string s = msg ? trim(U(msg)) : "";
    if (msg)
        LocalFree(msg);
    return s + " (" + std::to_string(e) + ")";
}

struct Handle {
    HANDLE h = INVALID_HANDLE_VALUE;
    Handle() = default;
    explicit Handle(HANDLE x) : h(x) {}
    ~Handle()
    {
        if (h != INVALID_HANDLE_VALUE && h)
            CloseHandle(h);
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    bool ok() const { return h != INVALID_HANDLE_VALUE && h; }
};

std::string physicalPath(DWORD n) { return "\\\\.\\PhysicalDrive" + std::to_string(n); }

bool parsePhysical(const std::string& p, DWORD& n)
{
    std::string s = lower(p);
    for (const char* pre : {"\\\\.\\physicaldrive", "physicaldrive", "\\\\?\\physicaldrive"})
        if (startsWith(s, pre)) {
            s = s.substr(std::strlen(pre));
            break;
        }
    if (s.empty() || s.find_first_not_of("0123456789") != std::string::npos)
        return false;
    n = DWORD(std::stoul(s));
    return true;
}

// One volume and where it lives.
struct VolumeExtent {
    std::wstring guidPath;   // "\\?\Volume{...}\"
    DWORD disk = 0;
    uint64_t offset = 0, length = 0;
};

std::vector<VolumeExtent> allVolumes()
{
    std::vector<VolumeExtent> v;
    wchar_t name[MAX_PATH];
    HANDLE f = FindFirstVolumeW(name, MAX_PATH);
    if (f == INVALID_HANDLE_VALUE)
        return v;
    do {
        std::wstring guid = name;
        std::wstring dev = guid;
        if (!dev.empty() && dev.back() == L'\\')
            dev.pop_back();
        Handle h(CreateFileW(dev.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr));
        if (!h.ok())
            continue;
        std::vector<uint8_t> buf(sizeof(VOLUME_DISK_EXTENTS) + 8 * sizeof(DISK_EXTENT));
        DWORD got = 0;
        if (!DeviceIoControl(h.h, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, nullptr, 0, buf.data(), DWORD(buf.size()), &got, nullptr))
            continue;
        auto* ext = reinterpret_cast<VOLUME_DISK_EXTENTS*>(buf.data());
        for (DWORD i = 0; i < ext->NumberOfDiskExtents; ++i)
            v.push_back({guid, ext->Extents[i].DiskNumber, uint64_t(ext->Extents[i].StartingOffset.QuadPart),
                         uint64_t(ext->Extents[i].ExtentLength.QuadPart)});
    } while (FindNextVolumeW(f, name, MAX_PATH));
    FindVolumeClose(f);
    return v;
}

std::string mountOf(const std::wstring& guid)
{
    wchar_t buf[1024] = {};
    DWORD len = 0;
    if (GetVolumePathNamesForVolumeNameW(guid.c_str(), buf, 1024, &len) && buf[0])
        return U(buf);
    return {};
}

std::vector<DWORD> systemDiskNumbers()
{
    std::vector<DWORD> out;
    wchar_t win[MAX_PATH];
    if (!GetSystemWindowsDirectoryW(win, MAX_PATH))
        return out;
    wchar_t vol[MAX_PATH];
    wchar_t root[4] = {win[0], L':', L'\\', 0};
    if (!GetVolumeNameForVolumeMountPointW(root, vol, MAX_PATH))
        return out;
    for (const VolumeExtent& e : allVolumes())
        if (_wcsicmp(e.guidPath.c_str(), vol) == 0)
            out.push_back(e.disk);
    return out;
}

bool describe(DWORD n, DriveInfo& d, const std::vector<VolumeExtent>& vols, const std::vector<DWORD>& sys)
{
    Handle h(CreateFileW(W(physicalPath(n)).c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr));
    if (!h.ok())
        return false;
    d = DriveInfo{};
    d.path = physicalPath(n);

    STORAGE_PROPERTY_QUERY q{};
    q.PropertyId = StorageDeviceProperty;
    q.QueryType = PropertyStandardQuery;
    std::vector<uint8_t> buf(4096);
    DWORD got = 0;
    BusType: {
        if (DeviceIoControl(h.h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof q, buf.data(), DWORD(buf.size()), &got, nullptr)) {
            auto* sd = reinterpret_cast<STORAGE_DEVICE_DESCRIPTOR*>(buf.data());
            auto str = [&](DWORD off) {
                return off && off < got ? trim(std::string(reinterpret_cast<const char*>(buf.data() + off))) : std::string();
            };
            d.vendor = str(sd->VendorIdOffset);
            d.model = str(sd->ProductIdOffset);
            d.serial = str(sd->SerialNumberOffset);
            d.removable = sd->RemovableMedia != FALSE;
            switch (sd->BusType) {
            case BusTypeUsb: d.bus = "USB"; d.usb = true; break;
            case BusTypeSd:
            case BusTypeMmc: d.bus = "SD"; d.removable = true; break;
            case BusTypeNvme: d.bus = "NVMe"; break;
            case BusTypeSata: d.bus = "SATA"; break;
            case BusTypeAta: d.bus = "ATA"; break;
            case BusTypeScsi: d.bus = "SCSI"; break;
            case BusTypeRAID: d.bus = "RAID"; break;
            case BusTypeVirtual:
            case BusTypeFileBackedVirtual: d.bus = "Virtual"; break;
            default: d.bus = "?";
            }
        }
    }
    DISK_GEOMETRY_EX geo{};
    if (DeviceIoControl(h.h, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, nullptr, 0, &geo, sizeof geo, &got, nullptr)) {
        d.size = uint64_t(geo.DiskSize.QuadPart);
        d.sectorSize = geo.Geometry.BytesPerSector ? geo.Geometry.BytesPerSector : 512;
        if (geo.Geometry.MediaType == RemovableMedia)
            d.removable = true;
    }
    if (!DeviceIoControl(h.h, IOCTL_DISK_IS_WRITABLE, nullptr, 0, nullptr, 0, &got, nullptr) && GetLastError() == ERROR_WRITE_PROTECT)
        d.readOnly = true;
    d.system = std::find(sys.begin(), sys.end(), n) != sys.end();
    d.id = d.serial.empty() ? d.path + ":" + std::to_string(d.size) : d.serial;

    for (const VolumeExtent& e : vols) {
        if (e.disk != n)
            continue;
        VolumeInfo v;
        std::wstring dev = e.guidPath;
        v.path = U(dev.c_str());
        v.offset = e.offset;
        v.size = e.length;
        v.mountPoint = mountOf(e.guidPath);
        wchar_t label[MAX_PATH + 1] = {}, fsName[MAX_PATH + 1] = {};
        if (GetVolumeInformationW(e.guidPath.c_str(), label, MAX_PATH, nullptr, nullptr, nullptr, fsName, MAX_PATH)) {
            v.label = U(label);
            v.fsType = lower(U(fsName));
        }
        d.volumes.push_back(v);
    }
    std::sort(d.volumes.begin(), d.volumes.end(), [](const VolumeInfo& a, const VolumeInfo& b) { return a.offset < b.offset; });
    return d.size > 0;
}

DWORD diskNumberOf(const std::string& path)
{
    DWORD n = 0;
    return parsePhysical(path, n) ? n : DWORD(-1);
}

// Opens a volume, locks it and dismounts it; the handle keeps the lock until closed.
HANDLE lockVolume(const std::wstring& guidPath, const JobContext& ctx)
{
    std::wstring dev = guidPath;
    if (!dev.empty() && dev.back() == L'\\')
        dev.pop_back();
    HANDLE h = CreateFileW(dev.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return nullptr;
    DWORD got = 0;
    bool locked = false;
    for (int i = 0; i < 40 && !locked; ++i) {
        locked = DeviceIoControl(h, FSCTL_LOCK_VOLUME, nullptr, 0, nullptr, 0, &got, nullptr) != FALSE;
        if (!locked)
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    if (!locked) {
        CloseHandle(h);
        throw Error(format(T("Cannot lock %1. Close any program or window that uses the drive and try again."), {U(guidPath.c_str())}));
    }
    DeviceIoControl(h, FSCTL_DISMOUNT_VOLUME, nullptr, 0, nullptr, 0, &got, nullptr);
    ctx.log(format(T("Locked and dismounted %1"), {U(guidPath.c_str())}));
    return h;
}

const VolumeExtent* volumeAt(const std::vector<VolumeExtent>& vols, DWORD disk, uint64_t offset)
{
    for (const VolumeExtent& e : vols)
        if (e.disk == disk && e.offset == offset)
            return &e;
    return nullptr;
}

// Aligned positional I/O on a raw handle (Windows requires whole sectors).
void handleIo(HANDLE h, bool write, uint64_t offset, void* buf, size_t len, uint32_t ss, const std::string& what)
{
    auto io = [&](uint64_t off, void* p, size_t n) {
        size_t done = 0;
        while (done < n) {
            OVERLAPPED ov{};
            ov.Offset = DWORD(off + done);
            ov.OffsetHigh = DWORD((off + done) >> 32);
            DWORD x = 0;
            const DWORD chunk = DWORD(std::min<size_t>(n - done, 32 * MiB));
            const BOOL r = write ? WriteFile(h, static_cast<char*>(p) + done, chunk, &x, &ov) : ReadFile(h, static_cast<char*>(p) + done, chunk, &x, &ov);
            if (!r || x == 0) {
                const DWORD e = GetLastError();
                if (write && (e == ERROR_DISK_FULL || e == ERROR_SECTOR_NOT_FOUND))
                    throw Error(T("The drive is full: the image is larger than the drive"));
                throw Error(format(write ? T("Write error at %1 on %2: %3") : T("Read error at %1 on %2: %3"),
                                   {humanBytes(off + done), what, winError(e)}));
            }
            done += x;
        }
    };
    if (offset % ss == 0 && len % ss == 0) {
        io(offset, buf, len);
        return;
    }
    // Read-modify-write the partial sectors at both ends.
    const uint64_t start = offset / ss * ss;
    const uint64_t end = (offset + len + ss - 1) / ss * ss;
    std::vector<uint8_t> tmp(size_t(end - start));
    if (write) {
        io(start, tmp.data(), ss);
        if (end - start > ss)
            io(end - ss, tmp.data() + (end - start - ss), ss);
        std::memcpy(tmp.data() + (offset - start), buf, len);
        // switch to write mode for the whole aligned range
        size_t done = 0;
        while (done < tmp.size()) {
            OVERLAPPED ov{};
            ov.Offset = DWORD(start + done);
            ov.OffsetHigh = DWORD((start + done) >> 32);
            DWORD x = 0;
            if (!WriteFile(h, tmp.data() + done, DWORD(std::min<size_t>(tmp.size() - done, 32 * MiB)), &x, &ov) || !x)
                throw Error(format(T("Write error at %1 on %2: %3"), {humanBytes(start + done), what, winError()}));
            done += x;
        }
    } else {
        io(start, tmp.data(), tmp.size());
        std::memcpy(buf, tmp.data() + (offset - start), len);
    }
}

// A locked volume used as a partition target.
class VolumeTarget final : public BlockTarget {
public:
    VolumeTarget(HANDLE h, uint64_t size, uint32_t ss, std::string name) : h_(h), size_(size), ss_(ss), name_(std::move(name)) {}
    ~VolumeTarget() override
    {
        DWORD got = 0;
        FlushFileBuffers(h_);
        DeviceIoControl(h_, FSCTL_UNLOCK_VOLUME, nullptr, 0, nullptr, 0, &got, nullptr);
        CloseHandle(h_);
    }
    void readAt(uint64_t o, void* b, size_t n) override { handleIo(h_, false, o, b, n, ss_, name_); }
    void writeAt(uint64_t o, const void* b, size_t n) override { handleIo(h_, true, o, const_cast<void*>(b), n, ss_, name_); }
    uint64_t size() const override { return size_; }
    uint32_t sectorSize() const override { return ss_; }
    void flush() override { FlushFileBuffers(h_); }

private:
    HANDLE h_;
    uint64_t size_;
    uint32_t ss_;
    std::string name_;
};

std::map<std::string, std::wstring> gAssignedLetters;   // mount point -> volume we gave a letter

} // namespace

// ---------------------------------------------------------------- listing

std::vector<DriveInfo> listDrives(const ListOptions& opt)
{
    std::vector<DriveInfo> out;
    const auto vols = allVolumes();
    const auto sys = systemDiskNumbers();
    for (DWORD n = 0; n < 64; ++n) {
        DriveInfo d;
        if (!describe(n, d, vols, sys))
            continue;
        const bool virt = d.bus == "Virtual";
        const bool portable = d.usb || d.removable || (virt && opt.includeVirtual);
        if (!portable && !opt.includeFixed)
            continue;
        out.push_back(d);
    }
    return out;
}

bool findDrive(const std::string& path, DriveInfo& out, bool allowFile)
{
    DWORD n = 0;
    if (parsePhysical(path, n))
        return describe(n, out, allVolumes(), systemDiskNumbers());
    if (!allowFile)
        return false;
    std::error_code ec;
    const fs::path p(std::u8string(path.begin(), path.end()));
    if (!fs::is_regular_file(p, ec))
        return false;
    out = DriveInfo{};
    out.path = path;
    out.id = path;
    out.model = U(p.filename().wstring().c_str());
    out.bus = "File";
    out.size = uint64_t(fs::file_size(p, ec));
    out.isFile = true;
    return true;
}

bool haveAdminRights()
{
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok))
        return false;
    TOKEN_ELEVATION el{};
    DWORD got = 0;
    const bool ok = GetTokenInformation(tok, TokenElevation, &el, sizeof el, &got) && el.TokenIsElevated;
    CloseHandle(tok);
    return ok;
}

// ---------------------------------------------------------------- programs

std::string findProgram(const std::string& name)
{
    wchar_t buf[MAX_PATH];
    std::wstring n = W(name);
    if (SearchPathW(nullptr, n.c_str(), L".exe", MAX_PATH, buf, nullptr))
        return U(buf);
    // Next to bootaz-cli.exe (bundled helpers such as wimlib-imagex.exe).
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    fs::path dir = fs::path(self).parent_path();
    for (const std::wstring& cand : {n, n + L".exe"}) {
        const fs::path p = dir / cand;
        if (fs::exists(p))
            return U(p.wstring().c_str());
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
    std::wstring cmd = L"\"" + W(exe) + L"\"";
    std::string shown = argv[0];
    for (size_t i = 1; i < argv.size(); ++i) {
        cmd += L" \"" + W(argv[i]) + L"\"";
        shown += " " + argv[i];
    }
    ctx.log("$ " + shown);
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    CreatePipe(&rd, &wr, &sa, 0);
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(0);
    if (!CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(rd);
        CloseHandle(wr);
        throw Error(format(T("Cannot start %1: %2"), {argv[0], winError()}));
    }
    CloseHandle(wr);
    std::string out;
    char buf[4096];
    DWORD n = 0;
    while (ReadFile(rd, buf, sizeof buf, &n, nullptr) && n)
        out.append(buf, n);
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    for (const std::string& l : split(out, '\n'))
        if (!trim(l).empty())
            ctx.log("  " + trim(l));
    if (output)
        *output = out;
    return int(code);
}

// ---------------------------------------------------------------- formatting (fmifs.dll)

namespace {

enum FmifsCallbackCommand { FCC_PROGRESS = 0, FCC_DONE = 11, FCC_ACCESS_DENIED = 6, FCC_MEDIA_WRITE_PROTECTED = 7,
                            FCC_VOLUME_IN_USE = 8, FCC_INCOMPATIBLE_FILE_SYSTEM = 3, FCC_BAD_LABEL = 12,
                            FCC_CLUSTER_SIZE_TOO_SMALL = 16, FCC_CLUSTER_SIZE_TOO_BIG = 17, FCC_VOLUME_TOO_SMALL = 18,
                            FCC_VOLUME_TOO_BIG = 19, FCC_DEVICE_NOT_READY = 24 };
using FmifsCallback = BOOLEAN(__stdcall*)(int command, DWORD subAction, PVOID info);
using FormatExFn = VOID(__stdcall*)(PWCHAR root, DWORD media, PWCHAR fs, PWCHAR label, BOOL quick, DWORD cluster, FmifsCallback cb);

int gFormatError = 0;
bool gFormatDone = false;
const JobContext* gFormatCtx = nullptr;

BOOLEAN __stdcall formatCallback(int command, DWORD, PVOID info)
{
    switch (command) {
    case FCC_PROGRESS:
        if (gFormatCtx && info)
            gFormatCtx->progress(T("Formatting"), *static_cast<DWORD*>(info), 100);
        break;
    case FCC_DONE:
        gFormatDone = true;
        if (info && !*static_cast<BOOLEAN*>(info) && !gFormatError)
            gFormatError = -1;
        break;
    case FCC_ACCESS_DENIED:
    case FCC_MEDIA_WRITE_PROTECTED:
    case FCC_VOLUME_IN_USE:
    case FCC_INCOMPATIBLE_FILE_SYSTEM:
    case FCC_BAD_LABEL:
    case FCC_CLUSTER_SIZE_TOO_SMALL:
    case FCC_CLUSTER_SIZE_TOO_BIG:
    case FCC_VOLUME_TOO_SMALL:
    case FCC_VOLUME_TOO_BIG:
    case FCC_DEVICE_NOT_READY:
        gFormatError = command;
        break;
    default:
        break;
    }
    return TRUE;
}

} // namespace

bool osCanFormat(const std::string& fsType) { return fsType == "fat32" || fsType == "ntfs" || fsType == "exfat"; }

void osFormat(Disk& disk, int index, uint64_t offset, uint64_t size, const std::string& fsType, const std::string& label,
              uint32_t cluster, const JobContext& ctx)
{
    (void)size;
    if (!osCanFormat(fsType))
        throw Error(format(T("Unsupported file system: %1"), {fsType}));
    // FormatEx reports through a plain callback, so only one format runs at a time.
    static std::mutex formatLock;
    std::lock_guard<std::mutex> lock(formatLock);
    const std::string vol = disk.partitionPath(index, offset, ctx);   // "\\?\Volume{...}\"
    HMODULE lib = LoadLibraryW(L"fmifs.dll");
    if (!lib)
        throw Error(T("fmifs.dll is missing"));
    auto formatEx = reinterpret_cast<FormatExFn>(reinterpret_cast<void*>(GetProcAddress(lib, "FormatEx")));
    if (!formatEx)
        throw Error(T("FormatEx is missing"));
    std::wstring root = W(vol), fsw = fsType == "ntfs" ? L"NTFS" : fsType == "exfat" ? L"exFAT" : L"FAT32", lab = W(label);
    gFormatError = 0;
    gFormatDone = false;
    gFormatCtx = &ctx;
    ctx.log(format(T("Formatting %1 as %2"), {vol, U(fsw.c_str())}));
    formatEx(root.data(), 0x0C /* fixed media */, fsw.data(), lab.data(), TRUE, cluster, formatCallback);
    gFormatCtx = nullptr;
    FreeLibrary(lib);
    if (gFormatError || !gFormatDone)
        throw Error(format(T("Formatting the drive as %1 failed (code %2)"), {fsType, std::to_string(gFormatError)}));
}

// ---------------------------------------------------------------- Disk

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
        d->h_ = CreateFileW(W(drive.path).c_str(), GENERIC_READ | (writable ? GENERIC_WRITE : 0), FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (d->h_ == INVALID_HANDLE_VALUE) {
            d->h_ = nullptr;
            throw Error(format(T("Cannot open %1: %2"), {drive.path, winError()}));
        }
        LARGE_INTEGER li{};
        GetFileSizeEx(d->h_, &li);
        d->size_ = uint64_t(li.QuadPart);
        return d;
    }
    if (writable) {
        // Lock and dismount every volume so nothing writes to the drive behind our back.
        const DWORD n = diskNumberOf(drive.path);
        for (const VolumeExtent& e : allVolumes())
            if (e.disk == n)
                if (HANDLE h = lockVolume(e.guidPath, ctx))
                    d->locks_.push_back(h);
    }
    d->h_ = CreateFileW(W(drive.path).c_str(), GENERIC_READ | (writable ? GENERIC_WRITE : 0), FILE_SHARE_READ | FILE_SHARE_WRITE,
                        nullptr, OPEN_EXISTING, writable ? FILE_FLAG_WRITE_THROUGH : 0, nullptr);
    if (d->h_ == INVALID_HANDLE_VALUE) {
        d->h_ = nullptr;
        const DWORD e = GetLastError();
        if (e == ERROR_ACCESS_DENIED)
            throw Error(T("Administrator rights are required to write to drives"));
        throw Error(format(T("Cannot open %1: %2"), {drive.path, winError(e)}));
    }
    DISK_GEOMETRY_EX geo{};
    DWORD got = 0;
    if (DeviceIoControl(d->h_, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, nullptr, 0, &geo, sizeof geo, &got, nullptr)) {
        d->size_ = uint64_t(geo.DiskSize.QuadPart);
        d->sector_ = geo.Geometry.BytesPerSector ? geo.Geometry.BytesPerSector : 512;
    }
    if (drive.size && d->size_ != drive.size)
        throw Error(T("The drive changed since it was selected; refresh the list and try again"));
    if (writable) {
        // Forget the old layout; the new one is written by us.
        DeviceIoControl(d->h_, IOCTL_DISK_DELETE_DRIVE_LAYOUT, nullptr, 0, nullptr, 0, &got, nullptr);
    }
    return d;
}

Disk::~Disk()
{
    for (const std::string& m : mounts_) {
        auto it = gAssignedLetters.find(m);
        if (it != gAssignedLetters.end()) {
            DeleteVolumeMountPointW(W(m).c_str());
            gAssignedLetters.erase(it);
        }
    }
    for (void* h : locks_) {
        DWORD got = 0;
        DeviceIoControl(h, FSCTL_UNLOCK_VOLUME, nullptr, 0, nullptr, 0, &got, nullptr);
        CloseHandle(h);
    }
    if (h_)
        CloseHandle(h_);
}

void Disk::rawRead(uint64_t offset, void* buf, size_t len) { handleIo(h_, false, offset, buf, len, sector_, drive_.path); }

void Disk::rawWrite(uint64_t offset, const void* buf, size_t len)
{
    handleIo(h_, true, offset, const_cast<void*>(buf), len, sector_, drive_.path);
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
    if (h_)
        FlushFileBuffers(h_);
}

void Disk::rescanPartitions(int expected, const JobContext& ctx)
{
    flush();
    if (drive_.isFile)
        return;
    // The old volumes are gone: drop their locks, then let Windows read the new table.
    for (void* h : locks_) {
        DWORD got = 0;
        DeviceIoControl(h, FSCTL_UNLOCK_VOLUME, nullptr, 0, nullptr, 0, &got, nullptr);
        CloseHandle(h);
    }
    locks_.clear();
    DWORD got = 0;
    ctx.log(T("Asking the system to read the new partition table"));
    DeviceIoControl(h_, IOCTL_DISK_UPDATE_PROPERTIES, nullptr, 0, nullptr, 0, &got, nullptr);
    const DWORD n = diskNumberOf(drive_.path);
    for (int i = 0; i < 60; ++i) {
        int seen = 0;
        for (const VolumeExtent& e : allVolumes())
            if (e.disk == n)
                ++seen;
        if (seen >= expected)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
}

std::string Disk::partitionPath(int index, uint64_t offset, const JobContext& ctx)
{
    (void)index;
    const DWORD n = diskNumberOf(drive_.path);
    for (int i = 0; i < 60; ++i) {
        const auto vols = allVolumes();
        if (const VolumeExtent* e = volumeAt(vols, n, offset))
            return U(e->guidPath.c_str());
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    (void)ctx;
    throw Error(T("Windows did not create a volume for the new partition"));
}

std::unique_ptr<BlockTarget> Disk::partition(int index, uint64_t offset, uint64_t size, const JobContext& ctx)
{
    (void)index;
    if (drive_.isFile)
        return std::make_unique<SubTarget>(*this, offset, size);
    // Windows refuses raw writes into the area of a mounted volume, so write through the volume
    // itself while it is locked.
    const DWORD n = diskNumberOf(drive_.path);
    const VolumeExtent* e = nullptr;
    std::vector<VolumeExtent> vols;
    for (int i = 0; i < 20 && !e; ++i) {
        vols = allVolumes();
        e = volumeAt(vols, n, offset);
        if (!e)
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    if (!e)
        return std::make_unique<SubTarget>(*this, offset, size);
    HANDLE h = lockVolume(e->guidPath, ctx);
    if (!h)
        return std::make_unique<SubTarget>(*this, offset, size);
    return std::make_unique<VolumeTarget>(h, size, sector_, U(e->guidPath.c_str()));
}

std::string Disk::mountPartition(int index, uint64_t offset, const std::string& fsType, const JobContext& ctx)
{
    (void)fsType;
    const std::string vol = partitionPath(index, offset, ctx);
    const std::wstring wv = W(vol);
    std::string mp = mountOf(wv);
    if (!mp.empty())
        return mp;
    // Give it a free drive letter for the copy.
    const DWORD used = GetLogicalDrives();
    for (int l = 'Z'; l >= 'D'; --l) {
        if (used & (1u << (l - 'A')))
            continue;
        const std::wstring root = {wchar_t(l), L':', L'\\'};
        if (SetVolumeMountPointW(root.c_str(), wv.c_str())) {
            mp = U(root.c_str());
            gAssignedLetters[mp] = wv;
            mounts_.push_back(mp);
            ctx.log(format(T("Mounted the new partition at %1"), {mp}));
            return mp;
        }
    }
    throw Error(T("No free drive letter to mount the new partition"));
}

void Disk::unmountPartition(const std::string& mountPoint, const JobContext& ctx)
{
    (void)ctx;
    // Flush the file system before anything else touches the volume.
    const std::wstring root = W(mountPoint);
    std::wstring dev = L"\\\\.\\" + root.substr(0, 2);
    HANDLE h = CreateFileW(dev.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(h);
        DWORD got = 0;
        DeviceIoControl(h, FSCTL_LOCK_VOLUME, nullptr, 0, nullptr, 0, &got, nullptr);
        DeviceIoControl(h, FSCTL_DISMOUNT_VOLUME, nullptr, 0, nullptr, 0, &got, nullptr);
        DeviceIoControl(h, FSCTL_UNLOCK_VOLUME, nullptr, 0, nullptr, 0, &got, nullptr);
        CloseHandle(h);
    }
    auto it = gAssignedLetters.find(mountPoint);
    if (it != gAssignedLetters.end()) {
        DeleteVolumeMountPointW(root.c_str());
        gAssignedLetters.erase(it);
    }
    mounts_.erase(std::remove(mounts_.begin(), mounts_.end(), mountPoint), mounts_.end());
}

namespace {

// "Safely remove": find the disk's device instance and ask its parent (the USB device) to eject.
bool ejectDisk(DWORD diskNumber)
{
    HDEVINFO set = SetupDiGetClassDevsW(&GUID_DEVINTERFACE_DISK, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE)
        return false;
    bool ok = false;
    SP_DEVICE_INTERFACE_DATA ifd{};
    ifd.cbSize = sizeof ifd;
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(set, nullptr, &GUID_DEVINTERFACE_DISK, i, &ifd); ++i) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &ifd, nullptr, 0, &need, nullptr);
        std::vector<uint8_t> buf(need);
        auto* det = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buf.data());
        det->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        SP_DEVINFO_DATA dev{};
        dev.cbSize = sizeof dev;
        if (!SetupDiGetDeviceInterfaceDetailW(set, &ifd, det, need, nullptr, &dev))
            continue;
        Handle h(CreateFileW(det->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr));
        STORAGE_DEVICE_NUMBER sdn{};
        DWORD got = 0;
        if (!h.ok() || !DeviceIoControl(h.h, IOCTL_STORAGE_GET_DEVICE_NUMBER, nullptr, 0, &sdn, sizeof sdn, &got, nullptr) ||
            sdn.DeviceNumber != diskNumber)
            continue;
        DEVINST parent = 0;
        if (CM_Get_Parent(&parent, dev.DevInst, 0) != CR_SUCCESS)
            parent = dev.DevInst;
        for (int tries = 0; tries < 3 && !ok; ++tries) {
            PNP_VETO_TYPE veto = PNP_VetoTypeUnknown;
            ok = CM_Request_Device_EjectW(parent, &veto, nullptr, 0, 0) == CR_SUCCESS;
            if (!ok)
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        break;
    }
    SetupDiDestroyDeviceInfoList(set);
    return ok;
}

} // namespace

void Disk::release(bool eject, const JobContext& ctx)
{
    for (const std::string& m : std::vector<std::string>(mounts_))
        unmountPartition(m, ctx);
    flush();
    DWORD got = 0;
    if (h_ && !drive_.isFile)
        DeviceIoControl(h_, IOCTL_DISK_UPDATE_PROPERTIES, nullptr, 0, nullptr, 0, &got, nullptr);
    for (void* h : locks_) {
        DeviceIoControl(h, FSCTL_UNLOCK_VOLUME, nullptr, 0, nullptr, 0, &got, nullptr);
        CloseHandle(h);
    }
    locks_.clear();
    if (h_) {
        CloseHandle(h_);
        h_ = nullptr;
    }
    if (eject && !drive_.isFile) {
        if (ejectDisk(diskNumberOf(drive_.path)))
            ctx.log(T("The drive was ejected and can be removed safely"));
        else
            ctx.log(T("The drive could not be ejected automatically; use \"Safely Remove Hardware\""));
    }
}

void handOverToUser(const std::string&) {}

std::string makePrivateTempDir(const std::string& prefix, uint64_t needBytes)
{
    wchar_t base[MAX_PATH];
    GetTempPathW(MAX_PATH, base);
    ULARGE_INTEGER avail{};
    if (needBytes && GetDiskFreeSpaceExW(base, &avail, nullptr, nullptr) && avail.QuadPart < needBytes)
        throw Error(format(T("Not enough free space for temporary files: %1 needed, %2 available"),
                           {humanBytes(needBytes), humanBytes(avail.QuadPart)}));
    for (int i = 0; i < 100; ++i) {
        const std::wstring p = std::wstring(base) + W(prefix) + L"-" + std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(GetCurrentProcessId() + i);
        if (CreateDirectoryW(p.c_str(), nullptr))   // fails if it exists: nobody can pre-create it for us
            return U(p.c_str());
    }
    throw Error(T("Cannot create a temporary folder"));
}

bool invokingUserMayWrite(const std::string&, std::string*) { return true; }

} // namespace bootaz
