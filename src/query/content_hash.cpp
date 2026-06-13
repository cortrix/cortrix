#include "cortrix/query/content_hash.h"

#include <openssl/sha.h>

#include <array>
#include <cctype>
#include <cstdint>

namespace cortrix::query {

namespace {

constexpr char kPrefix[] = "sha256:";
constexpr std::size_t kPrefixLen = 7;   // strlen("sha256:")
constexpr std::size_t kHexLen = 32;     // 16 bytes → 32 hex chars

inline char HexDigit(unsigned v) {
    return static_cast<char>(v < 10 ? '0' + v : 'a' + (v - 10));
}

inline int HexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// One uint64 → 16 big-endian hex chars appended to `out`.
void AppendBe64Hex(uint64_t v, std::string& out) {
    for (int shift = 60; shift >= 0; shift -= 4) {
        out.push_back(HexDigit(static_cast<unsigned>((v >> shift) & 0xF)));
    }
}

}  // namespace

std::string ContentHashToString(uint64_t hi, uint64_t lo) {
    std::string s;
    s.reserve(kPrefixLen + kHexLen);
    s.append(kPrefix);
    AppendBe64Hex(hi, s);
    AppendBe64Hex(lo, s);
    return s;
}

bool ContentHashFromString(const std::string& s, uint64_t* hi, uint64_t* lo) {
    if (s.size() != kPrefixLen + kHexLen) return false;
    if (s.compare(0, kPrefixLen, kPrefix) != 0) return false;

    uint64_t parts[2] = {0, 0};
    std::size_t pos = kPrefixLen;
    for (int part = 0; part < 2; ++part) {
        uint64_t acc = 0;
        for (int i = 0; i < 16; ++i) {
            int v = HexVal(s[pos++]);
            if (v < 0) return false;
            acc = (acc << 4) | static_cast<uint64_t>(v);
        }
        parts[part] = acc;
    }
    *hi = parts[0];
    *lo = parts[1];
    return true;
}

std::string ContentHashOfContent(const std::string& content) {
    std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
    SHA256(reinterpret_cast<const unsigned char*>(content.data()),
           content.size(), digest.data());
    // Take the first 16 bytes as big-endian hi/lo (matches ContentHashToString).
    uint64_t hi = 0, lo = 0;
    for (int i = 0; i < 8; ++i) hi = (hi << 8) | digest[i];
    for (int i = 8; i < 16; ++i) lo = (lo << 8) | digest[i];
    return ContentHashToString(hi, lo);
}

}  // namespace cortrix::query
