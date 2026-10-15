#pragma once
// Boot code written to the drive for legacy BIOS boot, and the UEFI:NTFS helper partition.

#include "Disk.h"

#include <cstdint>
#include <vector>

namespace bootaz {

std::vector<uint8_t> mbrWindows();       // Windows 7+ MBR: boots the active partition
std::vector<uint8_t> mbrSyslinux();      // Syslinux MBR (MBR scheme)
std::vector<uint8_t> mbrGptSyslinux();   // Syslinux gptmbr.bin (legacy boot from GPT)

// Turns a FAT32 volume into one that BIOS boots into Windows' bootmgr (keeps the BPB).
void writeFat32BootmgrRecord(BlockTarget& volume);
// Same for NTFS (keeps the BPB written by the formatter).
void writeNtfsBootmgrRecord(BlockTarget& volume);

// The 1 MiB FAT image of the UEFI:NTFS partition (NTFS / exFAT boot under UEFI).
std::vector<uint8_t> uefiNtfsImage();

} // namespace bootaz
