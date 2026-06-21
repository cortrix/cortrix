// Structural-invariant matrices for id/ulid.h GenerateUlid. ULIDs embed a
// 48-bit timestamp + 80-bit randomness, so exact values are non-deterministic;
// we assert ONLY the structural / ordering invariants the spec guarantees:
//  - length == 26
//  - every char is in Crockford base32 (0-9 A-Z minus I L O U)
//  - first char encodes the high timestamp bits and is bounded (48 bits in 10
//    base32 chars => leading char < '8' for any in-range ms)
//  - within one millisecond, successive mints are strictly increasing (monotonic
//    factory) and unique
//  - across many draws, all values are unique
//  - the 10-char timestamp prefix is monotonic non-decreasing as now_ms grows
//  - GenerateUlid() (wall clock) also satisfies length/charset/uniqueness
//
// SUITE PREFIX: UlidUtilMatrix* (globally unique).
#include <gtest/gtest.h>

#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "cortrix/id/ulid.h"

namespace cortrix::id {
namespace {

constexpr int kUlidLen = 26;
constexpr int kTimeChars = 10;

// Crockford base32 alphabet (no I, L, O, U). Mirrors the generator's table.
bool IsCrockford(char ch) {
    static const std::string kSet = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
    return kSet.find(ch) != std::string::npos;
}

void ExpectStructurallyValid(const std::string& u) {
    ASSERT_EQ(u.size(), static_cast<size_t>(kUlidLen)) << "ulid=" << u;
    for (char ch : u) {
        EXPECT_TRUE(IsCrockford(ch)) << "non-Crockford char in " << u;
    }
}

// ---- length + charset over a matrix of timestamps ----
class UlidUtilMatrixStructure : public ::testing::TestWithParam<int64_t> {};

TEST_P(UlidUtilMatrixStructure, LengthAndCharset) {
    std::string u = GenerateUlid(GetParam());
    ExpectStructurallyValid(u);
}

INSTANTIATE_TEST_SUITE_P(
    Cases, UlidUtilMatrixStructure,
    ::testing::Values<int64_t>(
        0,            // epoch
        1,
        1000,         // 1 second
        60000,        // 1 minute
        3600000,      // 1 hour
        86400000,     // 1 day
        1000000000LL,
        1609459200000LL,  // 2021-01-01
        1700000000000LL,  // 2023-ish
        1893456000000LL,  // 2030-ish
        -1,               // clamped to 0 internally
        -100000,          // clamped to 0
        281474976710655LL // (2^48 - 1) max 48-bit timestamp
        ));

// ---- leading timestamp char bound: 48 bits fit in 10 base32 chars, so the
//      most-significant char only uses 3 of its 5 bits => index < 8 => '0'..'7'.
class UlidUtilMatrixLeadChar : public ::testing::TestWithParam<int64_t> {};

TEST_P(UlidUtilMatrixLeadChar, LeadingCharBounded) {
    std::string u = GenerateUlid(GetParam());
    ASSERT_EQ(u.size(), static_cast<size_t>(kUlidLen));
    // First char index is in [0,7] for any valid 48-bit timestamp.
    EXPECT_GE(u[0], '0');
    EXPECT_LE(u[0], '7') << "lead char " << u[0] << " for ms=" << GetParam();
}

INSTANTIATE_TEST_SUITE_P(
    Cases, UlidUtilMatrixLeadChar,
    ::testing::Values<int64_t>(0, 1000, 86400000, 1700000000000LL,
                               1893456000000LL, 281474976710655LL));

// ---- timestamp prefix monotonic as ms grows ----
TEST(UlidUtilMatrixOrdering, TimestampPrefixNonDecreasing) {
    const std::vector<int64_t> times = {0,        1,         1000,
                                        60000,    3600000,   86400000,
                                        1000000000LL, 1609459200000LL,
                                        1700000000000LL, 1893456000000LL};
    std::string prev_prefix;
    for (int64_t t : times) {
        std::string u = GenerateUlid(t);
        ASSERT_EQ(u.size(), static_cast<size_t>(kUlidLen));
        std::string prefix = u.substr(0, kTimeChars);
        if (!prev_prefix.empty()) {
            // Crockford base32 is sorted by ASCII for the digit/letter ranges used,
            // and the alphabet order equals ASCII order across 0-9 then A-Z(minus
            // excluded), so lexicographic compare reflects numeric order here.
            EXPECT_LE(prev_prefix, prefix)
                << "prefix went backwards: " << prev_prefix << " -> " << prefix;
        }
        prev_prefix = prefix;
    }
}

// ---- monotonic + unique within the same millisecond (incremented randomness) ----
TEST(UlidUtilMatrixMonotonic, SameMsStrictlyIncreasingAndUnique) {
    const int64_t ms = 1700000000000LL;
    const int kDraws = 256;
    std::vector<std::string> ulids;
    ulids.reserve(kDraws);
    for (int i = 0; i < kDraws; ++i) ulids.push_back(GenerateUlid(ms));

    std::set<std::string> uniq;
    for (int i = 0; i < kDraws; ++i) {
        ExpectStructurallyValid(ulids[i]);
        // Same ms => identical 10-char timestamp prefix.
        EXPECT_EQ(ulids[i].substr(0, kTimeChars), ulids[0].substr(0, kTimeChars));
        uniq.insert(ulids[i]);
        if (i > 0) {
            // Monotonic factory: strictly increasing lexicographically.
            EXPECT_LT(ulids[i - 1], ulids[i])
                << "not increasing at i=" << i;
        }
    }
    EXPECT_EQ(uniq.size(), static_cast<size_t>(kDraws)) << "duplicates found";
}

// ---- uniqueness across many distinct-ms draws ----
TEST(UlidUtilMatrixUniqueness, ManyDistinctTimestampsUnique) {
    std::set<std::string> uniq;
    const int kN = 1000;
    for (int i = 0; i < kN; ++i) {
        std::string u = GenerateUlid(1600000000000LL + i);
        ExpectStructurallyValid(u);
        uniq.insert(u);
    }
    EXPECT_EQ(uniq.size(), static_cast<size_t>(kN));
}

// ---- wall-clock overload: structure + uniqueness over a burst ----
TEST(UlidUtilMatrixWallClock, BurstIsValidAndUnique) {
    std::set<std::string> uniq;
    const int kN = 500;
    for (int i = 0; i < kN; ++i) {
        std::string u = GenerateUlid();
        ExpectStructurallyValid(u);
        uniq.insert(u);
    }
    // All unique (monotonic factory guarantees this even within one ms).
    EXPECT_EQ(uniq.size(), static_cast<size_t>(kN));
}

}  // namespace
}  // namespace cortrix::id
