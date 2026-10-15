#pragma once
// MBR and GPT partition tables.

#include "Disk.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace bootaz {

enum class Scheme { Mbr, Gpt };
enum class PartType { Fat32, Ntfs, Exfat, Linux, Esp, BiosBoot };

struct PartSpec {
    uint64_t offset = 0;   // bytes, aligned to 1 MiB
    uint64_t size = 0;
    PartType type = PartType::Fat32;
    std::string name;      // GPT partition name
    bool active = false;   // MBR boot flag (legacy BIOS)
};

struct Layout {
    Scheme scheme = Scheme::Mbr;
    std::vector<PartSpec> parts;
};

const char* schemeName(Scheme s);
uint32_t crc32(const void* data, size_t len, uint32_t crc = 0);

// Clears old partition tables and file system signatures at both ends of the drive.
void wipeSignatures(BlockTarget& disk, const JobContext& ctx);
// Writes the table. `bootCode` (up to 440 bytes) goes to the start of the MBR when given.
void writePartitionTable(BlockTarget& disk, const Layout& layout, const std::vector<uint8_t>& bootCode = {});
// The first usable / last usable byte for partitions under this scheme.
uint64_t layoutStart();
uint64_t layoutEnd(uint64_t diskSize, Scheme s, uint32_t sector);

struct ExistingPartition { uint64_t offset = 0, size = 0; uint8_t mbrType = 0; std::string gptType; };
std::vector<ExistingPartition> readPartitionTable(BlockTarget& disk, Scheme* scheme = nullptr);

} // namespace bootaz
