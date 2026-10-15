#pragma once
// FAT32 formatter and a fast writer for a freshly formatted volume: files are laid out in
// contiguous clusters while they are copied, directories and the FAT are written at the end.
// Works on any BlockTarget (a partition of a raw drive), so no mounting and no OS formatter
// is needed, and FAT32 is not limited to 32 GB as it is with Windows' own tools.

#include "Disk.h"
#include "TreeWriter.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace bootaz {

struct Fat32Params {
    std::string label = "BOOTAZ";
    uint32_t clusterSize = 0;     // bytes; 0 = automatic
    uint32_t hiddenSectors = 0;   // partition start in sectors (BPB field, needed to boot)
    uint32_t volumeId = 0;        // 0 = derived from the time
};

uint32_t fat32DefaultCluster(uint64_t volumeBytes);
std::string fat32Label(const std::string& label);   // upper case, 11 chars, FAT-safe

class Fat32Writer final : public TreeWriter {
public:
    // Formats the target and prepares for writing files. Throws Error.
    Fat32Writer(BlockTarget& target, const Fat32Params& p, const JobContext& ctx);

    void makeDir(const std::string& path) override;
    void writeFile(const std::string& path, uint64_t size, const FileSource& source) override;
    void finish() override;
    uint64_t freeBytes() const override;
    bool supportsFileSize(uint64_t size) const override { return size < 4 * GiB; }
    void readBack(const std::string& path, uint64_t size, const FileSink& sink) override;

    uint32_t clusterBytes() const { return clusterBytes_; }

private:
    struct Rec {
        std::string longName;
        uint8_t shortName[11] = {};
        uint8_t caseFlags = 0;     // NT lower-case bits when the 8.3 name only differs in case
        bool needLfn = false;
        uint8_t attr = 0;
        int dir = -1;              // child directory node, or -1 for a file
        uint32_t cluster = 0;
        uint32_t size = 0;
    };
    struct DirNode {
        std::vector<Rec> recs;
        std::map<std::string, int> children;   // lower-case name -> dir node
        std::map<std::string, bool> longNames; // lower-case names used here
        std::map<std::string, bool> shortNames;
        std::vector<uint32_t> chain;           // clusters, assigned in finish()
        int parent = -1;
    };

    int dirFor(const std::string& path, bool create);
    void addRec(int dir, const std::string& name, uint8_t attr, int childDir, uint32_t cluster, uint32_t size);
    uint32_t allocate(uint32_t count);            // contiguous; returns first cluster
    uint64_t clusterOffset(uint32_t c) const;
    void setChain(const std::vector<uint32_t>& clusters);
    void serializeDir(int idx);

    BlockTarget& t_;
    const JobContext& ctx_;
    uint32_t bps_ = 512;
    uint32_t spc_ = 8;
    uint32_t clusterBytes_ = 4096;
    uint32_t reserved_ = 32;
    uint32_t fatSectors_ = 0;
    uint32_t clusters_ = 0;      // data clusters
    uint32_t next_ = 3;          // next free cluster (2 is the root)
    uint64_t dataStart_ = 0;     // byte offset of cluster 2
    std::vector<uint32_t> fat_;  // entries 0 .. highest used
    std::vector<DirNode> dirs_;
    std::map<std::string, std::pair<uint32_t, uint64_t>> files_;   // lower-case path -> first cluster, size
    std::string label_;
    bool finished_ = false;
    uint16_t date_ = 0, time_ = 0;
};

// Reads the BPB of a FAT32 volume (for tests and verification).
struct Fat32Info {
    bool ok = false;
    uint32_t bytesPerSector = 0, sectorsPerCluster = 0, totalSectors = 0, hiddenSectors = 0;
    std::string label;
};
Fat32Info readFat32Info(BlockTarget& t);

} // namespace bootaz
