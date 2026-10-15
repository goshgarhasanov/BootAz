// Core unit tests. Plain asserts: no framework needed on any platform.
// Optional: set BOOTAZ_TEST_DATA to a folder with sample images for the image-reading tests.

#include "Hash.h"
#include "I18n.h"
#include "Image.h"
#include "Iso.h"
#include "Util.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

using namespace bootaz;

static int gFailed = 0, gPassed = 0;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (cond) {                                                                    \
            ++gPassed;                                                                 \
        } else {                                                                       \
            ++gFailed;                                                                 \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
        }                                                                              \
    } while (0)

#define CHECK_EQ(a, b)                                                                                     \
    do {                                                                                                   \
        const auto va = (a);                                                                               \
        const auto vb = (b);                                                                               \
        if (va == vb) {                                                                                    \
            ++gPassed;                                                                                     \
        } else {                                                                                           \
            ++gFailed;                                                                                     \
            std::fprintf(stderr, "FAIL %s:%d: %s == %s\n", __FILE__, __LINE__, #a, #b);                    \
        }                                                                                                  \
    } while (0)

static std::string hashOf(HashAlgo a, const std::string& s)
{
    auto h = Hasher::create(a);
    h->update(s.data(), s.size());
    return h->finishHex();
}

static void testHashes()
{
    CHECK_EQ(hashOf(HashAlgo::Md5, ""), std::string("d41d8cd98f00b204e9800998ecf8427e"));
    CHECK_EQ(hashOf(HashAlgo::Md5, "abc"), std::string("900150983cd24fb0d6963f7d28e17f72"));
    CHECK_EQ(hashOf(HashAlgo::Sha1, "abc"), std::string("a9993e364706816aba3e25717850c26c9cd0d89d"));
    CHECK_EQ(hashOf(HashAlgo::Sha256, "abc"),
             std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    CHECK_EQ(hashOf(HashAlgo::Sha256, "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
             std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
    CHECK_EQ(hashOf(HashAlgo::Sha512, "abc"),
             std::string("ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
                         "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f"));
    // Million 'a' fed in odd-sized pieces exercises the block buffering.
    auto h = Hasher::create(HashAlgo::Sha256);
    const std::string chunk(997, 'a');
    size_t left = 1000000;
    while (left) {
        const size_t n = std::min(left, chunk.size());
        h->update(chunk.data(), n);
        left -= n;
    }
    CHECK_EQ(h->finishHex(), std::string("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
    auto m = Hasher::create(HashAlgo::Md5);
    m->update(std::string(1000000, 'a').data(), 1000000);
    CHECK_EQ(m->finishHex(), std::string("7707d6ae4e027c70eea2a935c2296f21"));
}

static void testUtil()
{
    CHECK_EQ(humanBytes(0), std::string("0 B"));
    CHECK_EQ(humanBytes(1536), std::string("1.5 KB"));
    CHECK_EQ(humanBytes(16ULL * GiB), std::string("16.0 GB"));
    CHECK_EQ(format("%2 / %1", {"a", "b"}), std::string("b / a"));
    CHECK_EQ(utf16ToUtf8(utf8ToUtf16("Azərbaycan ✓ 𝄞")), std::string("Azərbaycan ✓ 𝄞"));
    CHECK_EQ(utf8ToUtf16("ə").size(), size_t(1));
    CHECK_EQ(jsonEscape("a\"b\\\n"), std::string("a\\\"b\\\\\\n"));
}

static void testI18n()
{
    i18n::setLanguage(i18n::Lang::En);
    CHECK_EQ(T("Cancelled"), std::string("Cancelled"));
    i18n::setLanguage(i18n::Lang::Az);
    CHECK_EQ(T("Cancelled"), std::string("Ləğv edildi"));
    CHECK_EQ(T("No such text"), std::string("No such text"));
    i18n::setLanguage(i18n::Lang::En);
}

// Image tests use files made by tests/make_test_images.sh; skipped when absent.
static void testImages(const std::string& dir)
{
    namespace fs = std::filesystem;
    auto have = [&](const char* n) { return fs::exists(fs::path(dir) / n); };
    auto path = [&](const char* n) { return (fs::path(dir) / n).string(); };

    if (have("rr.iso")) {   // Rock Ridge + Joliet, hybrid with an EFI image
        IsoImage iso;
        std::string why;
        CHECK(iso.open(path("rr.iso"), &why));
        CHECK(iso.tree() == IsoImage::Tree::RockRidge);
        const int f = iso.find("/Deep/Dir/With/Long Name Folder/CamelCase-File.txt");
        CHECK(f >= 0);
        if (f >= 0) {
            const auto d = iso.readFile(iso.entries()[size_t(f)]);
            CHECK_EQ(std::string(d.begin(), d.end()), std::string("hello from rock ridge\n"));
        }
        const int big = iso.find("/big.bin");
        CHECK(big >= 0);
        if (big >= 0) {
            CHECK_EQ(iso.entries()[size_t(big)].size, uint64_t(5 * MiB + 123));
            auto hh = Hasher::create(HashAlgo::Sha256);
            iso.streamFile(iso.entries()[size_t(big)], [&](const uint8_t* p, size_t n) { hh->update(p, n); });
            CHECK_EQ(hh->finishHex(), trim(readTextFile(path("big.bin.sha256"))));
        }
        const int link = iso.find("/link-to-readme");
        CHECK(link >= 0 && iso.entries()[size_t(link)].symlink);
        const ImageInfo info = analyzeImage(path("rr.iso"));
        CHECK(info.kind == ImageKind::Iso);
        CHECK_EQ(info.label, std::string("BOOTAZ_TEST"));
        CHECK(!info.efiLoaders.empty());
        CHECK(info.elToritoEfi);
    }
    if (have("evil.iso")) {   // names with ".." or "/" must never reach the copy
        IsoImage iso;
        CHECK(iso.open(path("evil.iso")));
        bool clean = true;
        iso.walk([&](const IsoEntry& e) {
            if (e.name.find('/') != std::string::npos || e.name == ".." || e.path.find("/../") != std::string::npos)
                clean = false;
        });
        CHECK(clean);
        CHECK(iso.find("/Deep/Dir/With/Long Name Folder/CamelCase-File.txt") >= 0);
    }
    if (have("joliet.iso")) {
        IsoImage iso;
        CHECK(iso.open(path("joliet.iso")));
        CHECK(iso.tree() == IsoImage::Tree::Joliet);
        CHECK(iso.find("/Deep/Dir/With/Long Name Folder/CamelCase-File.txt") >= 0);
    }
    if (have("udf.iso")) {   // UDF bridge, like Windows install media
        IsoImage iso;
        std::string why;
        CHECK(iso.open(path("udf.iso"), &why));
        CHECK(iso.tree() == IsoImage::Tree::Udf);
        const int f = iso.find("/sources/install.wim");
        CHECK(f >= 0);
        if (f >= 0) {
            auto hh = Hasher::create(HashAlgo::Sha256);
            iso.streamFile(iso.entries()[size_t(f)], [&](const uint8_t* p, size_t n) { hh->update(p, n); });
            CHECK_EQ(hh->finishHex(), trim(readTextFile(path("install.wim.sha256"))));
        }
        CHECK(iso.find("/efi/boot/bootx64.efi") >= 0);
        const ImageInfo info = analyzeImage(path("udf.iso"));
        CHECK(info.isWindows);
        CHECK_EQ(info.windowsInstallFile, std::string("/sources/install.wim"));
    }
    if (have("alpine.iso")) {
        const ImageInfo info = analyzeImage(path("alpine.iso"));
        CHECK(info.kind == ImageKind::Iso);
        CHECK(info.isohybrid);
        CHECK(info.isLinux);
        CHECK_EQ(info.distro, std::string("Alpine Linux"));
        CHECK(!info.efiLoaders.empty());
        CHECK(info.elToritoBios);
        CHECK(info.ddRecommended());
    }
    for (const char* n : {"disk.img.gz", "disk.img.xz", "disk.img.zst", "disk.img.bz2"}) {
        if (!have(n))
            continue;
        const ImageInfo info = analyzeImage(path(n));
        CHECK(info.kind == ImageKind::Compressed);
        auto s = ImageStream::open(path(n), info.compression);
        auto hh = Hasher::create(HashAlgo::Sha256);
        std::vector<uint8_t> buf(300000);
        size_t r;
        while ((r = s->read(buf.data(), buf.size())) > 0)
            hh->update(buf.data(), r);
        CHECK_EQ(hh->finishHex(), trim(readTextFile(path("disk.img.sha256"))));
    }
}

int main(int argc, char** argv)
{
    testHashes();
    testUtil();
    testI18n();
    const char* data = std::getenv("BOOTAZ_TEST_DATA");
    if (argc > 1)
        data = argv[1];
    if (data)
        testImages(data);
    else
        std::printf("(image tests skipped: set BOOTAZ_TEST_DATA)\n");
    std::printf("%d passed, %d failed\n", gPassed, gFailed);
    return gFailed ? 1 : 0;
}
