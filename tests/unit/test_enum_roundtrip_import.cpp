#include <gtest/gtest.h>

#include <optional>
#include <set>
#include <string>
#include <vector>

#include "cortrix/import/import_types.h"

// Exhaustive matrices for the DB import enums (src/import/import_types.cpp).
//   TextStrategy:     ToString <-> ParseTextStrategy (round-trip). "template"
//                     and everything else -> nullopt (V1.0 V3 ruling 13).
//   ImportTaskStatus: ToString only (one-way; lifecycle status). We pin each of
//                     the 6 tokens + uniqueness; no Parse surface exists.
// Suite names unique with RtImport* prefix.
namespace cortrix::import {
namespace {

// ----------------------------------------------------------------------------
// TextStrategy round-trip (2 values).
// ----------------------------------------------------------------------------
const std::vector<TextStrategy>& AllTextStrategies() {
    static const std::vector<TextStrategy> v = {TextStrategy::kPerRow,
                                                TextStrategy::kMerge};
    return v;
}

class RtTextStrategyMatrix : public ::testing::TestWithParam<TextStrategy> {};

TEST_P(RtTextStrategyMatrix, ToStringParseRoundTrip) {
    const TextStrategy s = GetParam();
    const std::string token = ToString(s);
    EXPECT_FALSE(token.empty());
    const std::optional<TextStrategy> parsed = ParseTextStrategy(token);
    ASSERT_TRUE(parsed.has_value()) << "token did not parse: " << token;
    EXPECT_EQ(*parsed, s) << "round-trip mismatch for token: " << token;
}

INSTANTIATE_TEST_SUITE_P(AllValues, RtTextStrategyMatrix,
                         ::testing::ValuesIn(AllTextStrategies()));

TEST(RtTextStrategyTokens, ExactWireStrings) {
    EXPECT_STREQ(ToString(TextStrategy::kPerRow), "per_row");
    EXPECT_STREQ(ToString(TextStrategy::kMerge), "merge");
}

// "template" is deliberately rejected in V1.0; everything unknown -> nullopt.
class RtTextStrategyUnknownMatrix
    : public ::testing::TestWithParam<std::string> {};

TEST_P(RtTextStrategyUnknownMatrix, UnknownIsNullopt) {
    EXPECT_FALSE(ParseTextStrategy(GetParam()).has_value())
        << "unexpectedly parsed: " << GetParam();
}

INSTANTIATE_TEST_SUITE_P(Unknowns, RtTextStrategyUnknownMatrix,
                         ::testing::Values(std::string(""), "template",
                                           "PER_ROW", "Merge", "jinja",
                                           "per-row"));

// ----------------------------------------------------------------------------
// ImportTaskStatus: ToString only (6 values). Pin tokens + global uniqueness.
// ----------------------------------------------------------------------------
const std::vector<ImportTaskStatus>& AllImportStatuses() {
    static const std::vector<ImportTaskStatus> v = {
        ImportTaskStatus::kQueued,     ImportTaskStatus::kRunning,
        ImportTaskStatus::kCompleted,  ImportTaskStatus::kFailed,
        ImportTaskStatus::kCancelling, ImportTaskStatus::kCancelled};
    return v;
}

class RtImportStatusMatrix
    : public ::testing::TestWithParam<ImportTaskStatus> {};

TEST_P(RtImportStatusMatrix, TokenNonEmptyLowercase) {
    const std::string token = ToString(GetParam());
    EXPECT_FALSE(token.empty());
    for (char c : token) {
        EXPECT_TRUE((c >= 'a' && c <= 'z') || c == '_')
            << "non-lowercase token char in: " << token;
    }
}

INSTANTIATE_TEST_SUITE_P(AllValues, RtImportStatusMatrix,
                         ::testing::ValuesIn(AllImportStatuses()));

TEST(RtImportStatusTokens, ExactStringsAndUniqueness) {
    EXPECT_STREQ(ToString(ImportTaskStatus::kQueued), "queued");
    EXPECT_STREQ(ToString(ImportTaskStatus::kRunning), "running");
    EXPECT_STREQ(ToString(ImportTaskStatus::kCompleted), "completed");
    EXPECT_STREQ(ToString(ImportTaskStatus::kFailed), "failed");
    EXPECT_STREQ(ToString(ImportTaskStatus::kCancelling), "cancelling");
    EXPECT_STREQ(ToString(ImportTaskStatus::kCancelled), "cancelled");

    std::set<std::string> seen;
    for (ImportTaskStatus s : AllImportStatuses()) {
        EXPECT_TRUE(seen.insert(ToString(s)).second)
            << "duplicate import status token: " << ToString(s);
    }
    EXPECT_EQ(seen.size(), 6u);
}

}  // namespace
}  // namespace cortrix::import
