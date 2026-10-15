#include "File.h"
#include "I18n.h"
#include "Util.h"

#include <cerrno>
#include <cstring>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace bootaz {

File::File(File&& o) noexcept { *this = std::move(o); }

File& File::operator=(File&& o) noexcept
{
    if (this != &o) {
        close();
#ifdef _WIN32
        h_ = std::exchange(o.h_, nullptr);
#else
        fd_ = std::exchange(o.fd_, -1);
#endif
        path_ = std::move(o.path_);
    }
    return *this;
}

#ifdef _WIN32

namespace {
std::string winError()
{
    const DWORD e = GetLastError();
    wchar_t* msg = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, e, 0,
                   reinterpret_cast<wchar_t*>(&msg), 0, nullptr);
    std::u16string w = msg ? std::u16string(reinterpret_cast<char16_t*>(msg)) : u"";
    if (msg)
        LocalFree(msg);
    return trim(utf16ToUtf8(w)) + " (" + std::to_string(e) + ")";
}
} // namespace

void File::open(const std::string& path, Mode mode)
{
    close();
    path_ = path;
    const std::u16string w = utf8ToUtf16(path);
    const DWORD access = mode == Read ? GENERIC_READ : GENERIC_READ | GENERIC_WRITE;
    const DWORD disp = mode == Write ? CREATE_ALWAYS : OPEN_EXISTING;
    HANDLE h = CreateFileW(reinterpret_cast<const wchar_t*>(w.c_str()), access, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           disp, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        throw Error(format(T("Cannot open %1: %2"), {path, winError()}));
    h_ = h;
}

bool File::isOpen() const { return h_ != nullptr; }

void File::close()
{
    if (h_) {
        CloseHandle(h_);
        h_ = nullptr;
    }
}

size_t File::readAt(uint64_t offset, void* buf, size_t len)
{
    size_t got = 0;
    while (got < len) {
        OVERLAPPED ov{};
        const uint64_t off = offset + got;
        ov.Offset = DWORD(off);
        ov.OffsetHigh = DWORD(off >> 32);
        DWORD n = 0;
        const DWORD chunk = DWORD(std::min<size_t>(len - got, 1u << 30));
        if (!ReadFile(h_, static_cast<char*>(buf) + got, chunk, &n, &ov)) {
            if (GetLastError() == ERROR_HANDLE_EOF)
                break;
            throw Error(format(T("Read error on %1: %2"), {path_, winError()}));
        }
        if (n == 0)
            break;
        got += n;
    }
    return got;
}

void File::writeAt(uint64_t offset, const void* buf, size_t len)
{
    size_t put = 0;
    while (put < len) {
        OVERLAPPED ov{};
        const uint64_t off = offset + put;
        ov.Offset = DWORD(off);
        ov.OffsetHigh = DWORD(off >> 32);
        DWORD n = 0;
        const DWORD chunk = DWORD(std::min<size_t>(len - put, 1u << 30));
        if (!WriteFile(h_, static_cast<const char*>(buf) + put, chunk, &n, &ov) || n == 0)
            throw Error(format(T("Write error on %1: %2"), {path_, winError()}));
        put += n;
    }
}

uint64_t File::size() const
{
    LARGE_INTEGER li{};
    if (!GetFileSizeEx(h_, &li))
        return 0;
    return uint64_t(li.QuadPart);
}

void File::flush()
{
    if (h_)
        FlushFileBuffers(h_);
}

#else

void File::open(const std::string& path, Mode mode)
{
    close();
    path_ = path;
    const int flags = mode == Read ? O_RDONLY : mode == Write ? (O_RDWR | O_CREAT | O_TRUNC) : O_RDWR;
    fd_ = ::open(path.c_str(), flags | O_CLOEXEC, 0644);
    if (fd_ < 0)
        throw Error(format(T("Cannot open %1: %2"), {path, std::strerror(errno)}));
}

bool File::isOpen() const { return fd_ >= 0; }

void File::close()
{
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

size_t File::readAt(uint64_t offset, void* buf, size_t len)
{
    size_t got = 0;
    while (got < len) {
        const ssize_t n = ::pread(fd_, static_cast<char*>(buf) + got, len - got, off_t(offset + got));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            throw Error(format(T("Read error on %1: %2"), {path_, std::strerror(errno)}));
        }
        if (n == 0)
            break;
        got += size_t(n);
    }
    return got;
}

void File::writeAt(uint64_t offset, const void* buf, size_t len)
{
    size_t put = 0;
    while (put < len) {
        const ssize_t n = ::pwrite(fd_, static_cast<const char*>(buf) + put, len - put, off_t(offset + put));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            throw Error(format(T("Write error on %1: %2"), {path_, std::strerror(errno)}));
        }
        put += size_t(n);
    }
}

uint64_t File::size() const
{
    struct stat st {};
    if (::fstat(fd_, &st) != 0)
        return 0;
    return uint64_t(st.st_size);
}

void File::flush()
{
    if (fd_ >= 0)
        ::fsync(fd_);
}

#endif

void File::readExact(uint64_t offset, void* buf, size_t len)
{
    if (readAt(offset, buf, len) != len)
        throw Error(format(T("Unexpected end of file in %1"), {path_}));
}

} // namespace bootaz
