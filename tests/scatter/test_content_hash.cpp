#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#include "cortrix/query/content_hash.h"

// content_hash representation layer (the retrieval-types spec) — the dedup hash-table
// key form "sha256:<32-hex>" + roundtrip + content-addressed determinism.
namespace cortrix::query {
namespace {

TEST(ContentHashTest, ToStringFormatIsSha256Prefixed32Hex) {
    std::string s = ContentHashToString(0x0123456789abcdefULL, 0xfedcba9876543210ULL);
    EXPECT_EQ(s, "sha256:0123456789abcdeffedcba9876543210");
    EXPECT_EQ(s.size(), 7u + 32u);
}

TEST(ContentHashTest, ZeroIsAllZeros) {
    EXPECT_EQ(ContentHashToString(0, 0), "sha256:00000000000000000000000000000000");
}

TEST(ContentHashTest, RoundtripPreservesValue) {
    const uint64_t hi = 0xdeadbeefcafef00dULL;
    const uint64_t lo = 0x0011223344556677ULL;
    std::string s = ContentHashToString(hi, lo);
    uint64_t out_hi = 0, out_lo = 0;
    ASSERT_TRUE(ContentHashFromString(s, &out_hi, &out_lo));
    EXPECT_EQ(out_hi, hi);
    EXPECT_EQ(out_lo, lo);
}

TEST(ContentHashTest, FromStringRejectsMalformed) {
    uint64_t hi = 0, lo = 0;
    EXPECT_FALSE(ContentHashFromString("md5:abc", &hi, &lo));            // wrong prefix
    EXPECT_FALSE(ContentHashFromString("sha256:short", &hi, &lo));       // wrong length
    EXPECT_FALSE(ContentHashFromString("sha256:" + std::string(32, 'g'), &hi, &lo));  // non-hex
    EXPECT_FALSE(ContentHashFromString("", &hi, &lo));
}

// Content-addressed: identical text → identical hash (so two NS holding the same
// chunk collapse to one dedup key, B-simplified).
TEST(ContentHashTest, SameContentSameHash) {
    EXPECT_EQ(ContentHashOfContent("hello world"), ContentHashOfContent("hello world"));
}

TEST(ContentHashTest, DifferentContentDifferentHash) {
    EXPECT_NE(ContentHashOfContent("hello world"), ContentHashOfContent("hello world!"));
}

// The content hash is a well-formed "sha256:<32-hex>" string parseable back to raw.
TEST(ContentHashTest, OfContentIsWellFormed) {
    std::string s = ContentHashOfContent("some chunk text");
    EXPECT_EQ(s.rfind("sha256:", 0), 0u);
    EXPECT_EQ(s.size(), 7u + 32u);
    uint64_t hi = 0, lo = 0;
    EXPECT_TRUE(ContentHashFromString(s, &hi, &lo));
}

// Known SHA-256 vector: SHA256("abc") = ba7816bf8f01cfea414140de5dae2223...; we take
// the first 16 bytes → "ba7816bf8f01cfea414140de5dae2223".
TEST(ContentHashTest, KnownSha256VectorFirst16Bytes) {
    EXPECT_EQ(ContentHashOfContent("abc"), "sha256:ba7816bf8f01cfea414140de5dae2223");
}

}  // namespace
}  // namespace cortrix::query
