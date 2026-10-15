#pragma once
// Portable 64-bit file access with UTF-8 paths on every platform.

#include <cstddef>
#include <cstdint>
#include <string>

namespace bootaz {

class File {
public:
    enum Mode { Read, Write /* create / truncate */, ReadWrite /* existing */ };

    File() = default;
    File(const std::string& path, Mode mode) { open(path, mode); }
    ~File() { close(); }
    File(const File&) = delete;
    File& operator=(const File&) = delete;
    File(File&& o) noexcept;
    File& operator=(File&& o) noexcept;

    void open(const std::string& path, Mode mode);   // throws Error
    bool isOpen() const;
    void close();

    // Positional I/O; short reads only at end of file. Throw Error on I/O failure.
    size_t readAt(uint64_t offset, void* buf, size_t len);
    void readExact(uint64_t offset, void* buf, size_t len);   // throws if fewer bytes are available
    void writeAt(uint64_t offset, const void* buf, size_t len);
    uint64_t size() const;
    void flush();
    const std::string& path() const { return path_; }

private:
#ifdef _WIN32
    void* h_ = nullptr;   // HANDLE
#else
    int fd_ = -1;
#endif
    std::string path_;
};

} // namespace bootaz
