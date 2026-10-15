#pragma once
// Streaming MD5 / SHA-1 / SHA-256 / SHA-512, used to check images and to verify what was
// written. Self-contained so the core needs no crypto library on any platform.

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace bootaz {

enum class HashAlgo { Md5, Sha1, Sha256, Sha512 };

const char* hashName(HashAlgo a);          // "MD5", "SHA-1", ...
bool parseHashAlgo(const std::string& s, HashAlgo& out);  // "md5", "sha1", "sha256", "sha512"

class Hasher {
public:
    virtual ~Hasher() = default;
    virtual void update(const void* data, size_t len) = 0;
    virtual std::vector<uint8_t> finish() = 0;   // digest bytes; the object is spent afterwards
    std::string finishHex();

    static std::unique_ptr<Hasher> create(HashAlgo a);
};

// Feeds the same data to several hashers at once (one pass over an image for all digests).
class MultiHasher {
public:
    explicit MultiHasher(const std::vector<HashAlgo>& algos);
    void update(const void* data, size_t len);
    std::vector<std::pair<HashAlgo, std::string>> finishHex();

private:
    std::vector<std::pair<HashAlgo, std::unique_ptr<Hasher>>> hashers_;
};

std::string toHex(const uint8_t* data, size_t len);

} // namespace bootaz
