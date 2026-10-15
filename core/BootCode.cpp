#include "BootCode.h"
#include "I18n.h"

#include <cstring>

namespace bootaz {

namespace {
#include "../third_party/ms-sys/mbr_win7.h"
#include "../third_party/ms-sys/mbr_syslinux.h"
#include "../third_party/ms-sys/mbr_gpt_syslinux.h"
#include "../third_party/ms-sys/br_fat32_0x0.h"
#include "../third_party/ms-sys/br_fat32pe_0x52.h"
#include "../third_party/ms-sys/br_fat32pe_0x3f0.h"
#include "../third_party/ms-sys/br_fat32pe_0x1800.h"
#include "../third_party/ms-sys/br_ntfs_0x0.h"
#include "../third_party/ms-sys/br_ntfs_0x54.h"

std::vector<uint8_t> first440(const unsigned char* p, size_t n)
{
    return std::vector<uint8_t>(p, p + std::min<size_t>(n, 440));
}

// Writes `len` bytes at `off`, merging into whole sectors (the target may need aligned writes).
void patch(BlockTarget& t, uint64_t off, const unsigned char* data, size_t len)
{
    const uint32_t ss = t.sectorSize();
    const uint64_t start = off / ss * ss;
    const uint64_t end = (off + len + ss - 1) / ss * ss;
    std::vector<uint8_t> buf(size_t(end - start));
    t.readAt(start, buf.data(), buf.size());
    std::memcpy(buf.data() + (off - start), data, len);
    t.writeAt(start, buf.data(), buf.size());
}
} // namespace

// Defined in the generated uefiNtfsImage.cpp (tools/embed.cpp.in).
const unsigned char* uefiNtfsImageData();
size_t uefiNtfsImageSize();

std::vector<uint8_t> mbrWindows() { return first440(mbr_win7_0x0, sizeof mbr_win7_0x0); }
std::vector<uint8_t> mbrSyslinux() { return first440(mbr_syslinux_0x0, sizeof mbr_syslinux_0x0); }
std::vector<uint8_t> mbrGptSyslinux() { return first440(mbr_gpt_syslinux_0x0, sizeof mbr_gpt_syslinux_0x0); }

void writeFat32BootmgrRecord(BlockTarget& v)
{
    patch(v, 0x0, br_fat32_0x0, sizeof br_fat32_0x0);
    patch(v, 0x52, br_fat32pe_0x52, sizeof br_fat32pe_0x52);
    patch(v, 0x3f0, br_fat32pe_0x3f0, sizeof br_fat32pe_0x3f0);
    patch(v, 0x1800, br_fat32pe_0x1800, sizeof br_fat32pe_0x1800);
    v.flush();
}

void writeNtfsBootmgrRecord(BlockTarget& v)
{
    patch(v, 0x0, br_ntfs_0x0, sizeof br_ntfs_0x0);
    patch(v, 0x54, br_ntfs_0x54, sizeof br_ntfs_0x54);
    v.flush();
}

std::vector<uint8_t> uefiNtfsImage()
{
    std::vector<uint8_t> img(uefiNtfsImageData(), uefiNtfsImageData() + uefiNtfsImageSize());
    // Name the partition after BootAz (FAT12/16 label: boot sector and the root folder entry).
    const char label[12] = "BOOTAZ_UEFI";
    if (img.size() > 512 && img[510] == 0x55 && img[511] == 0xAA) {
        const uint32_t bps = img[11] | (img[12] << 8);
        const uint32_t reserved = img[14] | (img[15] << 8);
        const uint32_t fats = img[16];
        const uint32_t rootEntries = img[17] | (img[18] << 8);
        const uint32_t fatSize = img[22] | (img[23] << 8);
        if (img[38] == 0x29)
            std::memcpy(img.data() + 43, label, 11);
        const uint64_t root = uint64_t(reserved + fats * fatSize) * bps;
        for (uint32_t i = 0; i < rootEntries && root + (i + 1) * 32ULL <= img.size(); ++i) {
            uint8_t* e = img.data() + root + i * 32ULL;
            if (e[0] == 0)
                break;
            if (e[0] != 0xE5 && e[11] == 0x08) {
                std::memcpy(e, label, 11);
                break;
            }
        }
    }
    return img;
}

} // namespace bootaz
