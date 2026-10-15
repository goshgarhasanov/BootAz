#pragma once
// Destination for the files of an ISO: our own FAT32 writer or a mounted volume.

#include "Util.h"

#include <cstdint>
#include <functional>
#include <string>

namespace bootaz {

// Calls `sink` with the file's bytes in order (chunked).
using FileSink = std::function<void(const uint8_t*, size_t)>;
using FileSource = std::function<void(const FileSink&)>;

class TreeWriter {
public:
    virtual ~TreeWriter() = default;
    virtual void makeDir(const std::string& path) = 0;   // "/EFI/BOOT"; parents are created as needed
    virtual void writeFile(const std::string& path, uint64_t size, const FileSource& source) = 0;
    virtual void finish() = 0;
    virtual uint64_t freeBytes() const = 0;
    virtual bool supportsFileSize(uint64_t size) const = 0;
    // Reads a written file back (verification).
    virtual void readBack(const std::string& path, uint64_t size, const FileSink& sink) = 0;
};

// Writes into a folder of a mounted volume (NTFS / exFAT / ext on Linux, any volume on Windows).
class DirTreeWriter final : public TreeWriter {
public:
    DirTreeWriter(const std::string& root, uint64_t freeSpace, bool fat) : root_(root), free_(freeSpace), fat_(fat) {}
    void makeDir(const std::string& path) override;
    void writeFile(const std::string& path, uint64_t size, const FileSource& source) override;
    void finish() override;
    uint64_t freeBytes() const override { return free_; }
    bool supportsFileSize(uint64_t size) const override { return !fat_ || size < 4 * GiB; }
    void readBack(const std::string& path, uint64_t size, const FileSink& sink) override;

private:
    std::string root_;
    uint64_t free_;
    bool fat_;
};

} // namespace bootaz
