#include "Partition.h"
#include "I18n.h"

#include <algorithm>
#include <cstring>
#include <random>

namespace bootaz {

namespace {

inline void put16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); }
inline void put32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (8 * i)); }
inline void put64(uint8_t* p, uint64_t v) { for (int i = 0; i < 8; ++i) p[i] = uint8_t(v >> (8 * i)); }
inline uint32_t get32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24); }
inline uint64_t get64(const uint8_t* p) { return get32(p) | (uint64_t(get32(p + 4)) << 32); }

using Guid = std::array<uint8_t, 16>;

// "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7" -> on-disk bytes (first three fields little endian).
Guid guidFromString(const char* s)
{
    std::string hex;
    for (const char* c = s; *c; ++c)
        if (*c != '-')
            hex += *c;
    uint8_t b[16];
    for (int i = 0; i < 16; ++i)
        b[i] = uint8_t(std::stoi(hex.substr(size_t(i) * 2, 2), nullptr, 16));
    return {b[3], b[2], b[1], b[0], b[5], b[4], b[7], b[6], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]};
}

std::string guidToString(const uint8_t* g)
{
    char buf[40];
    std::snprintf(buf, sizeof buf, "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X", g[3], g[2], g[1], g[0],
                  g[5], g[4], g[7], g[6], g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
    return buf;
}

Guid randomGuid()
{
    static std::random_device rd;
    static std::mt19937_64 rng(rd());
    Guid g;
    for (size_t i = 0; i < 16; i += 8) {
        const uint64_t r = rng();
        std::memcpy(g.data() + i, &r, 8);
    }
    g[7] = uint8_t((g[7] & 0x0f) | 0x40);   // version 4 (field stored little endian)
    g[8] = uint8_t((g[8] & 0x3f) | 0x80);
    return g;
}

Guid typeGuid(PartType t)
{
    switch (t) {
    case PartType::Esp: return guidFromString("C12A7328-F81F-11D2-BA4B-00A0C93EC93B");
    case PartType::Linux: return guidFromString("0FC63DAF-8483-4772-8E79-3D69D8477DE4");
    case PartType::BiosBoot: return guidFromString("21686148-6449-6E6F-744E-656564454649");
    default: return guidFromString("EBD0A0A2-B9E5-4433-87C0-68B6B72699C7");   // Microsoft basic data
    }
}

uint8_t mbrType(PartType t)
{
    switch (t) {
    case PartType::Fat32: return 0x0C;   // FAT32 LBA
    case PartType::Ntfs:
    case PartType::Exfat: return 0x07;
    case PartType::Linux: return 0x83;
    case PartType::Esp: return 0xEF;
    case PartType::BiosBoot: return 0xDA;
    }
    return 0x0C;
}

// CHS for an LBA (or the "beyond 8 GB" marker) as stored in MBR entries.
void chs(uint8_t* p, uint64_t lba)
{
    if (lba >= 1024ULL * 255 * 63) {
        p[0] = 0xFE;
        p[1] = 0xFF;
        p[2] = 0xFF;
        return;
    }
    const uint32_t c = uint32_t(lba / (255 * 63));
    const uint32_t h = uint32_t((lba / 63) % 255);
    const uint32_t s = uint32_t(lba % 63 + 1);
    p[0] = uint8_t(h);
    p[1] = uint8_t(s | ((c >> 2) & 0xC0));
    p[2] = uint8_t(c);
}

void mbrEntry(uint8_t* e, uint8_t type, uint64_t lba, uint64_t count, bool active)
{
    e[0] = active ? 0x80 : 0x00;
    chs(e + 1, lba);
    e[4] = type;
    chs(e + 5, lba + count - 1);
    put32(e + 8, uint32_t(std::min<uint64_t>(lba, 0xFFFFFFFF)));
    put32(e + 12, uint32_t(std::min<uint64_t>(count, 0xFFFFFFFF)));
}

} // namespace

const char* schemeName(Scheme s) { return s == Scheme::Gpt ? "GPT" : "MBR"; }

uint32_t crc32(const void* data, size_t len, uint32_t crc)
{
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? 0xEDB88320 ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        init = true;
    }
    crc = ~crc;
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < len; ++i)
        crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

uint64_t layoutStart() { return 1 * MiB; }

uint64_t layoutEnd(uint64_t diskSize, Scheme s, uint32_t sector)
{
    uint64_t end = diskSize;
    if (s == Scheme::Gpt)
        end -= 33ULL * sector + sector;   // backup entries + header
    return end / MiB * MiB;
}

void wipeSignatures(BlockTarget& disk, const JobContext& ctx)
{
    ctx.log(T("Clearing old partition tables"));
    const uint64_t size = disk.size();
    std::vector<uint8_t> zero(1 * MiB, 0);
    disk.writeAt(0, zero.data(), size_t(std::min<uint64_t>(zero.size(), size)));
    if (size > 2 * MiB) {
        const uint64_t tail = (size - 1 * MiB) / disk.sectorSize() * disk.sectorSize();
        disk.writeAt(tail, zero.data(), size_t(size - tail));
    }
}

void writePartitionTable(BlockTarget& disk, const Layout& layout, const std::vector<uint8_t>& bootCode)
{
    const uint32_t ss = disk.sectorSize();
    const uint64_t totalSectors = disk.size() / ss;
    std::vector<uint8_t> mbr(ss, 0);
    if (!bootCode.empty())
        std::memcpy(mbr.data(), bootCode.data(), std::min<size_t>(bootCode.size(), 440));

    if (layout.scheme == Scheme::Mbr) {
        if (layout.parts.size() > 4)
            throw Error("MBR holds at most four partitions");
        std::random_device rd;
        put32(mbr.data() + 440, uint32_t(rd()));   // disk signature
        for (size_t i = 0; i < layout.parts.size(); ++i) {
            const PartSpec& p = layout.parts[i];
            mbrEntry(mbr.data() + 446 + 16 * i, mbrType(p.type), p.offset / ss, p.size / ss, p.active);
        }
        mbr[510] = 0x55;
        mbr[511] = 0xAA;
        disk.writeAt(0, mbr.data(), ss);
        disk.flush();
        return;
    }

    // ---- GPT
    if (layout.parts.size() > 128)
        throw Error("Too many partitions");
    const uint32_t entrySize = 128, entryCount = 128;
    const uint64_t entrySectors = (uint64_t(entrySize) * entryCount + ss - 1) / ss;
    std::vector<uint8_t> entries(size_t(entrySectors * ss), 0);
    for (size_t i = 0; i < layout.parts.size(); ++i) {
        const PartSpec& p = layout.parts[i];
        uint8_t* e = entries.data() + i * entrySize;
        const Guid t = typeGuid(p.type), u = randomGuid();
        std::memcpy(e, t.data(), 16);
        std::memcpy(e + 16, u.data(), 16);
        put64(e + 32, p.offset / ss);
        put64(e + 40, (p.offset + p.size) / ss - 1);
        put64(e + 48, p.type == PartType::Esp ? 0 : 0);
        const std::u16string name = utf8ToUtf16(p.name).substr(0, 36);
        for (size_t k = 0; k < name.size(); ++k)
            put16(e + 56 + 2 * k, uint16_t(name[k]));
    }
    const uint32_t entriesCrc = crc32(entries.data(), size_t(entrySize) * entryCount);
    const Guid diskGuid = randomGuid();

    auto header = [&](uint64_t self, uint64_t other, uint64_t entriesLba) {
        std::vector<uint8_t> h(ss, 0);
        std::memcpy(h.data(), "EFI PART", 8);
        put32(h.data() + 8, 0x00010000);
        put32(h.data() + 12, 92);
        put64(h.data() + 24, self);
        put64(h.data() + 32, other);
        put64(h.data() + 40, 2 + entrySectors);                       // first usable
        put64(h.data() + 48, totalSectors - 2 - entrySectors);        // last usable
        std::memcpy(h.data() + 56, diskGuid.data(), 16);
        put64(h.data() + 72, entriesLba);
        put32(h.data() + 80, entryCount);
        put32(h.data() + 84, entrySize);
        put32(h.data() + 88, entriesCrc);
        put32(h.data() + 16, crc32(h.data(), 92));
        return h;
    };

    // Protective MBR covering the whole drive.
    mbrEntry(mbr.data() + 446, 0xEE, 1, std::min<uint64_t>(totalSectors - 1, 0xFFFFFFFF), false);
    mbr[510] = 0x55;
    mbr[511] = 0xAA;
    disk.writeAt(0, mbr.data(), ss);

    const uint64_t lastLba = totalSectors - 1;
    const uint64_t backupEntries = lastLba - entrySectors;
    disk.writeAt(1ULL * ss, header(1, lastLba, 2).data(), ss);
    disk.writeAt(2ULL * ss, entries.data(), entries.size());
    disk.writeAt(backupEntries * ss, entries.data(), entries.size());
    disk.writeAt(lastLba * ss, header(lastLba, 1, backupEntries).data(), ss);
    disk.flush();
}

std::vector<ExistingPartition> readPartitionTable(BlockTarget& disk, Scheme* scheme)
{
    std::vector<ExistingPartition> out;
    const uint32_t ss = disk.sectorSize();
    std::vector<uint8_t> s0(ss), s1(ss);
    disk.readAt(0, s0.data(), ss);
    disk.readAt(ss, s1.data(), ss);
    if (!std::memcmp(s1.data(), "EFI PART", 8)) {
        if (scheme)
            *scheme = Scheme::Gpt;
        const uint64_t lba = get64(s1.data() + 72);
        const uint32_t n = get32(s1.data() + 80), es = get32(s1.data() + 84);
        if (es < 128 || n > 1024)
            return out;
        std::vector<uint8_t> e(size_t(n) * es);
        disk.readAt(lba * ss, e.data(), e.size());
        for (uint32_t i = 0; i < n; ++i) {
            const uint8_t* p = e.data() + size_t(i) * es;
            if (std::all_of(p, p + 16, [](uint8_t b) { return b == 0; }))
                continue;
            ExistingPartition x;
            x.gptType = guidToString(p);
            x.offset = get64(p + 32) * ss;
            x.size = (get64(p + 40) + 1) * ss - x.offset;
            out.push_back(x);
        }
        return out;
    }
    if (scheme)
        *scheme = Scheme::Mbr;
    if (s0[510] != 0x55 || s0[511] != 0xAA)
        return out;
    for (int i = 0; i < 4; ++i) {
        const uint8_t* p = s0.data() + 446 + 16 * i;
        if (p[4] == 0)
            continue;
        ExistingPartition x;
        x.mbrType = p[4];
        x.offset = uint64_t(get32(p + 8)) * ss;
        x.size = uint64_t(get32(p + 12)) * ss;
        out.push_back(x);
    }
    return out;
}

} // namespace bootaz
