#pragma once
// Reads the file tree of an optical disc image: ISO 9660 with Rock Ridge or Joliet names, and UDF
// (1.02 – 2.60, including the metadata partition) as used by Windows install images. Also reads
// the El Torito boot catalog. Pure C++ so it behaves the same on Linux and Windows.

#include "File.h"
#include "Util.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace bootaz {

struct IsoExtent {
    uint64_t offset = 0;   // byte offset in the image; kSparse = not recorded (reads as zeros)
    uint64_t length = 0;
    static constexpr uint64_t kSparse = ~0ULL;
};

struct IsoEntry {
    std::string name;
    std::string path;              // "/EFI/BOOT/BOOTX64.EFI"
    bool dir = false;
    bool symlink = false;
    std::string linkTarget;
    uint64_t size = 0;
    std::vector<IsoExtent> extents;
    std::vector<uint8_t> inlineData;   // UDF files stored inside their file entry
    std::vector<int> children;         // indices into IsoImage::entries()
    int parent = -1;
};

struct ElToritoEntry {
    uint8_t platform = 0;      // 0 = BIOS, 0xEF = UEFI
    uint8_t media = 0;         // 0 = no emulation
    uint32_t lba = 0;          // in 2048-byte sectors
    uint32_t sectors512 = 0;   // as recorded (0 or 1 often means "until the end of the image file")
    bool bootable = false;
};

class IsoImage {
public:
    enum class Tree { None, Iso9660, RockRidge, Joliet, Udf };

    // Returns false (with `why`) when the file is not an ISO 9660 / UDF image.
    bool open(const std::string& path, std::string* why = nullptr);

    const std::vector<IsoEntry>& entries() const { return entries_; }
    const IsoEntry& root() const { return entries_[0]; }
    int find(const std::string& path) const;   // case-insensitive; -1 if missing
    Tree tree() const { return tree_; }
    const char* treeName() const;
    std::string volumeLabel() const { return label_; }
    std::string isoLabel() const { return pvdLabel_; }   // ISO 9660 primary label (what boot configs refer to)
    bool hasUdf() const { return hasUdf_; }
    bool hasIso9660() const { return hasIso_; }
    const std::vector<ElToritoEntry>& bootEntries() const { return boot_; }
    uint64_t imageSize() const { return file_.size(); }

    // Reads a whole file (small files: configs, boot images).
    std::vector<uint8_t> readFile(const IsoEntry& e, uint64_t maxBytes = 64 * MiB);
    // Streams a file in chunks to `sink` (large files).
    void streamFile(const IsoEntry& e, const std::function<void(const uint8_t*, size_t)>& sink, const JobContext* ctx = nullptr);
    // Raw sectors (for El Torito images).
    std::vector<uint8_t> readRaw(uint64_t offset, size_t len);

    void walk(const std::function<void(const IsoEntry&)>& fn) const;   // depth first, root excluded

private:
    bool readIso9660(std::string* why);
    bool readUdf();
    void readElTorito(uint32_t catalogLba);
    int addEntry(IsoEntry e);
    void parseIsoDir(int parent, uint32_t lba, uint32_t size, bool joliet, bool rockRidge, int depth);

    struct UdfPartition { uint16_t number = 0; uint32_t start = 0; uint32_t length = 0; };
    struct UdfMap { int type = 1; uint16_t partition = 0; bool metadata = false; uint32_t metaFileLoc = 0; };
    uint64_t udfBlockToOffset(uint16_t partRef, uint32_t lbn);
    bool udfReadEntry(uint16_t partRef, uint32_t lbn, IsoEntry& e, uint8_t& fileType, int depth);
    void udfParseDir(int parent, const IsoEntry& dirEntry, uint16_t partRef, int depth);
    std::vector<uint8_t> entryData(const IsoEntry& e, uint64_t maxBytes);

    File file_;
    std::vector<IsoEntry> entries_;
    std::vector<ElToritoEntry> boot_;
    std::string label_;
    std::string pvdLabel_;
    Tree tree_ = Tree::None;
    bool hasUdf_ = false;
    bool hasIso_ = false;
    int rrSkip_ = 0;   // SUSP "SP" skip length

    uint32_t udfBlock_ = 2048;
    std::vector<UdfPartition> udfParts_;
    std::vector<UdfMap> udfMaps_;
    std::vector<IsoExtent> udfMetaExtents_;   // metadata partition: logical block -> image bytes
    uint16_t udfMetaPhysRef_ = 0;
};

} // namespace bootaz
