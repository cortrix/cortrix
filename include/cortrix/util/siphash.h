#pragma once
#include <cstddef>
#include <cstdint>

// SipHash-2-4 (Jean-Philippe Aumasson & Daniel J. Bernstein) — a fast, keyed 64-bit
// pseudo-random function. This is the reference algorithm (the authors released it
// into the public domain / CC0), rewritten as a single header-only inline function.
//
// Why Cortrix self-implements it (ARCH §1.8.4, D1 ruling):
//   block_id (uint64 P-HNSW label) = SipHash24(business_ulid, k0, k1). The hash must
//   be byte-for-byte STABLE across process restarts and library upgrades, otherwise
//   the persisted P-HNSW index would silently point at the wrong nodes. abseil's
//   absl::Hash explicitly does NOT guarantee cross-run stability (and abseil is not a
//   Cortrix dependency anyway), so we pin the fixed SipHash-2-4 standard instead.
//
// Determinism contract (ARCH §1.8.1): 8-byte message words are assembled little-endian
// by explicit shifts (never a raw reinterpret of host memory), so the output is
// identical on any byte order — a hard requirement for a persistent on-disk index.

namespace cortrix::util {

/// SipHash-2-4 keyed hash of `len` bytes at `data` under the 128-bit key (k0, k1).
/// Same (data, len, k0, k1) always yields the same 64-bit value, on any platform.
inline uint64_t SipHash24(const void* data, size_t len, uint64_t k0, uint64_t k1) {
    auto rotl = [](uint64_t x, int b) -> uint64_t { return (x << b) | (x >> (64 - b)); };

    uint64_t v0 = 0x736f6d6570736575ULL ^ k0;  // "somepseu"
    uint64_t v1 = 0x646f72616e646f6dULL ^ k1;  // "dorandom"
    uint64_t v2 = 0x6c7967656e657261ULL ^ k0;  // "lygenera"
    uint64_t v3 = 0x7465646279746573ULL ^ k1;  // "tedbytes"

    auto sipround = [&]() {
        v0 += v1; v1 = rotl(v1, 13); v1 ^= v0; v0 = rotl(v0, 32);
        v2 += v3; v3 = rotl(v3, 16); v3 ^= v2;
        v0 += v3; v3 = rotl(v3, 21); v3 ^= v0;
        v2 += v1; v1 = rotl(v1, 17); v1 ^= v2; v2 = rotl(v2, 32);
    };

    const auto* in = static_cast<const uint8_t*>(data);
    const size_t left = len & 7;          // trailing bytes that don't fill a word
    const uint8_t* const end = in + (len - left);

    // Full 8-byte words, little-endian, 2 compression rounds each (the "2" of 2-4).
    for (; in != end; in += 8) {
        uint64_t m = 0;
        for (int i = 0; i < 8; ++i) m |= static_cast<uint64_t>(in[i]) << (8 * i);
        v3 ^= m;
        sipround();
        sipround();
        v0 ^= m;
    }

    // Final block: the remaining `left` bytes, with the total length in the top byte.
    uint64_t b = static_cast<uint64_t>(len) << 56;
    for (size_t i = 0; i < left; ++i) b |= static_cast<uint64_t>(in[i]) << (8 * i);
    v3 ^= b;
    sipround();
    sipround();
    v0 ^= b;

    // Finalization: 4 rounds (the "4" of 2-4).
    v2 ^= 0xff;
    sipround();
    sipround();
    sipround();
    sipround();

    return v0 ^ v1 ^ v2 ^ v3;
}

}  // namespace cortrix::util
