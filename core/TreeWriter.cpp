#include "TreeWriter.h"
#include "File.h"
#include "I18n.h"

#include <filesystem>

namespace fs = std::filesystem;

namespace bootaz {

namespace {
fs::path u8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }
} // namespace

void DirTreeWriter::makeDir(const std::string& path)
{
    std::error_code ec;
    fs::create_directories(u8(root_) / u8(path.empty() || path[0] != '/' ? path : path.substr(1)), ec);
    if (ec)
        throw Error(format(T("Cannot create folder %1: %2"), {path, ec.message()}));
}

void DirTreeWriter::writeFile(const std::string& path, uint64_t size, const FileSource& source)
{
    if (!supportsFileSize(size))
        throw Error(format(T("%1 is larger than 4 GB, which FAT32 cannot store"), {path}));
    const fs::path full = u8(root_) / u8(path.empty() || path[0] != '/' ? path : path.substr(1));
    std::error_code ec;
    fs::create_directories(full.parent_path(), ec);
    const auto u = full.u8string();
    File out(std::string(u.begin(), u.end()), File::Write);
    uint64_t pos = 0;
    source([&](const uint8_t* p, size_t n) {
        out.writeAt(pos, p, n);
        pos += n;
    });
    if (pos != size)
        throw Error(format(T("%1 is shorter than expected"), {path}));
    free_ = free_ > size ? free_ - size : 0;
}

void DirTreeWriter::finish() {}

void DirTreeWriter::readBack(const std::string& path, uint64_t size, const FileSink& sink)
{
    const fs::path full = u8(root_) / u8(path.empty() || path[0] != '/' ? path : path.substr(1));
    const auto u = full.u8string();
    File in(std::string(u.begin(), u.end()), File::Read);
    if (in.size() != size)
        throw Error(format(T("%1 has the wrong size on the drive"), {path}));
    std::vector<uint8_t> buf(4 * MiB);
    for (uint64_t done = 0; done < size;) {
        const size_t n = in.readAt(done, buf.data(), size_t(std::min<uint64_t>(buf.size(), size - done)));
        if (n == 0)
            throw Error(format(T("%1 is shorter than expected"), {path}));
        sink(buf.data(), n);
        done += n;
    }
}

} // namespace bootaz
