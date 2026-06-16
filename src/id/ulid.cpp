#include <cstdint>
#include "cortrix/id/ulid.h"

#include <array>
#include <chrono>
#include <mutex>
#include <random>

namespace cortrix::id {
namespace {

// Crockford's base32 alphabet (excludes I, L, O, U — see ULID spec).
constexpr char kCrockford[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

// A ULID is 10 chars of 48-bit timestamp + 16 chars of 80-bit randomness.
constexpr int kTimeChars = 10;
constexpr int kRandChars = 16;
constexpr int kUlidChars = kTimeChars + kRandChars;  // 26

std::mutex g_mu;
int64_t g_last_ms = -1;
std::array<uint8_t, 10> g_last_rand{};  // 80 bits of randomness from the last mint

// Fill `out` (10 bytes) with fresh randomness.
void FreshRandom(std::array<uint8_t, 10>& out) {
    static thread_local std::mt19937_64 rng(std::random_device{}());
    std::uniform_int_distribution<unsigned int> dist(0, 255);
    for (auto& b : out) b = static_cast<uint8_t>(dist(rng));
}

// Increment the 80-bit big-endian random value by 1 (monotonic factory). On the
// astronomically unlikely all-0xFF overflow, just reseed — still time-ordered
// because the timestamp prefix advances on the next millisecond.
void IncrementRandom(std::array<uint8_t, 10>& r) {
    for (int i = static_cast<int>(r.size()) - 1; i >= 0; --i) {
        if (++r[i] != 0) return;  // no carry out of this byte
    }
    FreshRandom(r);  // overflowed all bytes
}

// Encode `value` (low `bits` bits significant) as `n` Crockford chars, MSB-first.
void EncodeBase32(uint64_t value, int bits, int n, std::string* out) {
    for (int i = n - 1; i >= 0; --i) {
        int shift = i * 5;
        unsigned idx = (shift < bits) ? static_cast<unsigned>((value >> shift) & 0x1F) : 0u;
        out->push_back(kCrockford[idx]);
    }
}

// Encode 80 bits (10 bytes, big-endian) as 16 Crockford chars. Splits into a high
// 40-bit half (chars 0..7) and a low 40-bit half (chars 8..15) so we never need
// 128-bit arithmetic.
void EncodeRandom(const std::array<uint8_t, 10>& r, std::string* out) {
    uint64_t hi = 0, lo = 0;  // 5 bytes (40 bits) each
    for (int i = 0; i < 5; ++i) hi = (hi << 8) | r[i];
    for (int i = 5; i < 10; ++i) lo = (lo << 8) | r[i];
    EncodeBase32(hi, 40, 8, out);
    EncodeBase32(lo, 40, 8, out);
}

}  // namespace

std::string GenerateUlid(int64_t now_ms) {
    if (now_ms < 0) now_ms = 0;

    std::array<uint8_t, 10> rand_bytes{};
    {
        std::lock_guard<std::mutex> lock(g_mu);
        if (now_ms == g_last_ms) {
            // Same millisecond as the previous mint: increment for monotonicity.
            IncrementRandom(g_last_rand);
        } else {
            g_last_ms = now_ms;
            FreshRandom(g_last_rand);
        }
        rand_bytes = g_last_rand;
    }

    std::string out;
    out.reserve(kUlidChars);
    EncodeBase32(static_cast<uint64_t>(now_ms), 48, kTimeChars, &out);
    EncodeRandom(rand_bytes, &out);
    return out;
}

std::string GenerateUlid() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const int64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    return GenerateUlid(ms);
}

}  // namespace cortrix::id
