#include <gtest/gtest.h>

#include <algorithm>
#include <set>
#include <string>
#include <vector>

#include "cortrix/id/ulid.h"

namespace cortrix::id {
namespace {

constexpr char kCrockford[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

bool IsCrockford(const std::string& s) {
    return std::all_of(s.begin(), s.end(), [](char c) {
        return std::string(kCrockford).find(c) != std::string::npos;
    });
}

TEST(UlidTest, LengthAndAlphabet) {
    std::string u = GenerateUlid(1700000000000);
    EXPECT_EQ(u.size(), 26u);
    EXPECT_TRUE(IsCrockford(u)) << u;
}

TEST(UlidTest, Uniqueness) {
    std::set<std::string> seen;
    for (int i = 0; i < 10000; ++i) {
        EXPECT_TRUE(seen.insert(GenerateUlid(1700000000000)).second);
    }
    EXPECT_EQ(seen.size(), 10000u);
}

TEST(UlidTest, MonotonicWithinSameMs) {
    // Same timestamp: successive ULIDs must be strictly increasing (the random
    // component is incremented), so a batch minted in one tick sorts in mint order.
    std::vector<std::string> ids;
    for (int i = 0; i < 1000; ++i) ids.push_back(GenerateUlid(1700000000000));
    for (size_t i = 1; i < ids.size(); ++i) {
        EXPECT_LT(ids[i - 1], ids[i]) << "at index " << i;
    }
}

TEST(UlidTest, TimeOrderedAcrossMs) {
    std::string earlier = GenerateUlid(1700000000000);
    std::string later = GenerateUlid(1700000001000);
    EXPECT_LT(earlier, later);
}

TEST(UlidTest, TimestampPrefixEncodesValue) {
    // A larger timestamp yields a lexicographically larger 10-char prefix.
    std::string a = GenerateUlid(1);
    std::string b = GenerateUlid(2);
    EXPECT_LT(a.substr(0, 10), b.substr(0, 10));
}

TEST(UlidTest, NegativeTimestampClampedToZero) {
    std::string u = GenerateUlid(-5);
    EXPECT_EQ(u.size(), 26u);
    EXPECT_EQ(u.substr(0, 10), std::string(10, '0'));  // time=0
}

TEST(UlidTest, CurrentClockOverloadWorks) {
    std::string u = GenerateUlid();
    EXPECT_EQ(u.size(), 26u);
    EXPECT_TRUE(IsCrockford(u));
}

}  // namespace
}  // namespace cortrix::id
