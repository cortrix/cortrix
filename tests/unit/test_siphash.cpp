#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "cortrix/util/siphash.h"

namespace cortrix::util {
namespace {

// Official SipHash-2-4 reference test vectors (Aumasson & Bernstein, public domain).
// Key = bytes 00..0f little-endian (k0=0x0706050403020100, k1=0x0f0e0d0c0b0a0908);
// for input length L the input is {0x00, 0x01, ..., L-1}. The 64 expected outputs
// cover every length 0..63, exercising every trailing-byte branch (left = 0..7).
constexpr uint64_t kK0 = 0x0706050403020100ULL;
constexpr uint64_t kK1 = 0x0f0e0d0c0b0a0908ULL;

const uint64_t kVectors[64] = {
    0x726fdb47dd0e0e31ULL, 0x74f839c593dc67fdULL, 0x0d6c8009d9a94f5aULL, 0x85676696d7fb7e2dULL,
    0xcf2794e0277187b7ULL, 0x18765564cd99a68dULL, 0xcbc9466e58fee3ceULL, 0xab0200f58b01d137ULL,
    0x93f5f5799a932462ULL, 0x9e0082df0ba9e4b0ULL, 0x7a5dbbc594ddb9f3ULL, 0xf4b32f46226bada7ULL,
    0x751e8fbc860ee5fbULL, 0x14ea5627c0843d90ULL, 0xf723ca908e7af2eeULL, 0xa129ca6149be45e5ULL,
    0x3f2acc7f57c29bdbULL, 0x699ae9f52cbe4794ULL, 0x4bc1b3f0968dd39cULL, 0xbb6dc91da77961bdULL,
    0xbed65cf21aa2ee98ULL, 0xd0f2cbb02e3b67c7ULL, 0x93536795e3a33e88ULL, 0xa80c038ccd5ccec8ULL,
    0xb8ad50c6f649af94ULL, 0xbce192de8a85b8eaULL, 0x17d835b85bbb15f3ULL, 0x2f2e6163076bcfadULL,
    0xde4daaaca71dc9a5ULL, 0xa6a2506687956571ULL, 0xad87a3535c49ef28ULL, 0x32d892fad841c342ULL,
    0x7127512f72f27cceULL, 0xa7f32346f95978e3ULL, 0x12e0b01abb051238ULL, 0x15e034d40fa197aeULL,
    0x314dffbe0815a3b4ULL, 0x027990f029623981ULL, 0xcadcd4e59ef40c4dULL, 0x9abfd8766a33735cULL,
    0x0e3ea96b5304a7d0ULL, 0xad0c42d6fc585992ULL, 0x187306c89bc215a9ULL, 0xd4a60abcf3792b95ULL,
    0xf935451de4f21df2ULL, 0xa9538f0419755787ULL, 0xdb9acddff56ca510ULL, 0xd06c98cd5c0975ebULL,
    0xe612a3cb9ecba951ULL, 0xc766e62cfcadaf96ULL, 0xee64435a9752fe72ULL, 0xa192d576b245165aULL,
    0x0a8787bf8ecb74b2ULL, 0x81b3e73d20b49b6fULL, 0x7fa8220ba3b2eceaULL, 0x245731c13ca42499ULL,
    0xb78dbfaf3a8d83bdULL, 0xea1ad565322a1a0bULL, 0x60e61c23a3795013ULL, 0x6606d7e446282b93ULL,
    0x6ca4ecb15c5f91e1ULL, 0x9f626da15c9625f3ULL, 0xe51b38608ef25f57ULL, 0x958a324ceb064572ULL,
};

TEST(SipHash24Test, OfficialVectorsAllLengths) {
    std::vector<uint8_t> in;
    for (int len = 0; len < 64; ++len) {
        EXPECT_EQ(SipHash24(in.data(), in.size(), kK0, kK1), kVectors[len])
            << "official SipHash-2-4 vector mismatch at input length " << len;
        in.push_back(static_cast<uint8_t>(len));
    }
}

// The single vector printed in the SipHash paper appendix (input length 15).
TEST(SipHash24Test, PaperAppendixVector) {
    uint8_t in[15];
    for (int i = 0; i < 15; ++i) in[i] = static_cast<uint8_t>(i);
    EXPECT_EQ(SipHash24(in, sizeof(in), kK0, kK1), 0xa129ca6149be45e5ULL);
}

// Stability is the whole point: the same (key, input) must yield the same output on
// every call, so block_id = SipHash24(ulid) survives process restarts (ARCH).
TEST(SipHash24Test, DeterministicAcrossCalls) {
    const std::string ulid = "01HQ8Z9K7M3N5P7R9T1V3W5X7Y";  // 26-char ULID shape
    EXPECT_EQ(SipHash24(ulid.data(), ulid.size(), kK0, kK1),
              SipHash24(ulid.data(), ulid.size(), kK0, kK1));
}

// A different per-deployment key yields a different mapping (key isolation, V4).
TEST(SipHash24Test, KeyDependent) {
    const std::string ulid = "01HQ8Z9K7M3N5P7R9T1V3W5X7Y";
    EXPECT_NE(SipHash24(ulid.data(), ulid.size(), kK0, kK1),
              SipHash24(ulid.data(), ulid.size(), kK0 ^ 1ULL, kK1));
}

// Length is part of the hashed input, and the empty input is well-defined — the
// HashChildIdToBlockId contract hashes id.size() bytes (so length matters, ARCH M4).
TEST(SipHash24Test, LengthSensitiveAndEmpty) {
    EXPECT_NE(SipHash24("AB", 1, kK0, kK1), SipHash24("AB", 2, kK0, kK1));
    EXPECT_EQ(SipHash24("", 0, kK0, kK1), kVectors[0]);
}

}  // namespace
}  // namespace cortrix::util
