#include <gtest/gtest.h>

#include <set>
#include <string>
#include <vector>

#include "cortrix/memory/memory_extractor.h"
#include "cortrix/memory/memory_transparency.h"
#include "cortrix/memory/opt_out_manager.h"

// Exhaustive matrices for the memory extraction/memory enums.
//   MemoryType:   ToString <-> ParseMemoryType (round-trip, case-insensitive on
//                 the 3 known values). Unknown -> kUnknown (caller coerces to
//                 kEvent per D4). src/memory/memory_extractor.cpp.
//   MemoryStatus: ToString only (3 values).
//   OptOutActor:  ToString only (4 values, cortrix::memory::immunity).
//   TriggeredBy:  ToString only (4 values, cortrix::memory::transparency).
// Suite names unique with RtMem* prefix.

namespace cortrix::memory {
namespace {

// ----------------------------------------------------------------------------
// MemoryType round-trip (kFact / kPreference / kEvent round-trip; kUnknown
// stringifies to "unknown" but is the parse sink, so handled separately).
// ----------------------------------------------------------------------------
const std::vector<MemoryType>& RoundTrippableMemoryTypes() {
    static const std::vector<MemoryType> v = {
        MemoryType::kFact, MemoryType::kPreference, MemoryType::kEvent};
    return v;
}

class RtMemoryTypeMatrix : public ::testing::TestWithParam<MemoryType> {};

TEST_P(RtMemoryTypeMatrix, ToStringParseRoundTrip) {
    const MemoryType type = GetParam();
    const std::string token = ToString(type);
    EXPECT_FALSE(token.empty());
    EXPECT_EQ(ParseMemoryType(token), type)
        << "round-trip mismatch for token: " << token;
}

TEST_P(RtMemoryTypeMatrix, ParseIsCaseInsensitive) {
    const MemoryType type = GetParam();
    std::string upper = ToString(type);
    for (char& c : upper) c = static_cast<char>(::toupper((unsigned char)c));
    EXPECT_EQ(ParseMemoryType(upper), type)
        << "case-insensitive parse failed for: " << upper;
}

INSTANTIATE_TEST_SUITE_P(AllValues, RtMemoryTypeMatrix,
                         ::testing::ValuesIn(RoundTrippableMemoryTypes()));

TEST(RtMemoryTypeTokens, ExactStrings) {
    EXPECT_STREQ(ToString(MemoryType::kFact), "fact");
    EXPECT_STREQ(ToString(MemoryType::kPreference), "preference");
    EXPECT_STREQ(ToString(MemoryType::kEvent), "event");
    EXPECT_STREQ(ToString(MemoryType::kUnknown), "unknown");
}

// Unknown LLM `type` -> kUnknown (the parse sink). "unknown" itself is NOT one of
// the 3 recognized values, so it also parses to kUnknown.
class RtMemoryTypeUnknownMatrix
    : public ::testing::TestWithParam<std::string> {};

TEST_P(RtMemoryTypeUnknownMatrix, UnrecognizedParsesToUnknown) {
    EXPECT_EQ(ParseMemoryType(GetParam()), MemoryType::kUnknown)
        << "unexpected non-unknown for: " << GetParam();
}

INSTANTIATE_TEST_SUITE_P(Unknowns, RtMemoryTypeUnknownMatrix,
                         ::testing::Values(std::string(""), "unknown", "facts",
                                           "prefs", "episodic", "memory"));

// ----------------------------------------------------------------------------
// MemoryStatus: ToString only (3 values). Pin tokens + uniqueness.
// ----------------------------------------------------------------------------
const std::vector<MemoryStatus>& AllMemoryStatuses() {
    static const std::vector<MemoryStatus> v = {
        MemoryStatus::kActive, MemoryStatus::kTentative,
        MemoryStatus::kInvalidated};
    return v;
}

class RtMemoryStatusMatrix : public ::testing::TestWithParam<MemoryStatus> {};

TEST_P(RtMemoryStatusMatrix, TokenNonEmptyLowercase) {
    const std::string token = ToString(GetParam());
    EXPECT_FALSE(token.empty());
    for (char c : token) {
        EXPECT_TRUE((c >= 'a' && c <= 'z') || c == '_')
            << "non-lowercase token char in: " << token;
    }
}

INSTANTIATE_TEST_SUITE_P(AllValues, RtMemoryStatusMatrix,
                         ::testing::ValuesIn(AllMemoryStatuses()));

TEST(RtMemoryStatusTokens, ExactStringsAndUniqueness) {
    EXPECT_STREQ(ToString(MemoryStatus::kActive), "active");
    EXPECT_STREQ(ToString(MemoryStatus::kTentative), "tentative");
    EXPECT_STREQ(ToString(MemoryStatus::kInvalidated), "invalidated");

    std::set<std::string> seen;
    for (MemoryStatus s : AllMemoryStatuses()) {
        EXPECT_TRUE(seen.insert(ToString(s)).second);
    }
    EXPECT_EQ(seen.size(), 3u);
}

}  // namespace
}  // namespace cortrix::memory

// ----------------------------------------------------------------------------
// OptOutActor: ToString only (4 values, cortrix::memory::immunity).
// ----------------------------------------------------------------------------
namespace cortrix::memory::immunity {
namespace {

const std::vector<OptOutActor>& AllOptOutActors() {
    static const std::vector<OptOutActor> v = {
        OptOutActor::kUserManual, OptOutActor::kAgentAuto,
        OptOutActor::kSystemAuto, OptOutActor::kTest};
    return v;
}

class RtOptOutActorMatrix : public ::testing::TestWithParam<OptOutActor> {};

TEST_P(RtOptOutActorMatrix, TokenNonEmptyLowercase) {
    const std::string token = ToString(GetParam());
    EXPECT_FALSE(token.empty());
    for (char c : token) {
        EXPECT_TRUE((c >= 'a' && c <= 'z') || c == '_')
            << "non-lowercase token char in: " << token;
    }
}

INSTANTIATE_TEST_SUITE_P(AllValues, RtOptOutActorMatrix,
                         ::testing::ValuesIn(AllOptOutActors()));

TEST(RtOptOutActorTokens, ExactStringsAndUniqueness) {
    EXPECT_STREQ(ToString(OptOutActor::kUserManual), "user_manual");
    EXPECT_STREQ(ToString(OptOutActor::kAgentAuto), "agent_auto");
    EXPECT_STREQ(ToString(OptOutActor::kSystemAuto), "system_auto");
    EXPECT_STREQ(ToString(OptOutActor::kTest), "test");

    std::set<std::string> seen;
    for (OptOutActor a : AllOptOutActors()) {
        EXPECT_TRUE(seen.insert(ToString(a)).second);
    }
    EXPECT_EQ(seen.size(), 4u);
}

}  // namespace
}  // namespace cortrix::memory::immunity

// ----------------------------------------------------------------------------
// TriggeredBy: ToString only (4 values, cortrix::memory::transparency).
// ----------------------------------------------------------------------------
namespace cortrix::memory::transparency {
namespace {

const std::vector<TriggeredBy>& AllTriggeredBy() {
    static const std::vector<TriggeredBy> v = {
        TriggeredBy::kUserManual, TriggeredBy::kUserEdit,
        TriggeredBy::kAdminRevoke, TriggeredBy::kLlmAuto};
    return v;
}

class RtTriggeredByMatrix : public ::testing::TestWithParam<TriggeredBy> {};

TEST_P(RtTriggeredByMatrix, TokenNonEmptyLowercase) {
    const std::string token = ToString(GetParam());
    EXPECT_FALSE(token.empty());
    for (char c : token) {
        EXPECT_TRUE((c >= 'a' && c <= 'z') || c == '_')
            << "non-lowercase token char in: " << token;
    }
}

INSTANTIATE_TEST_SUITE_P(AllValues, RtTriggeredByMatrix,
                         ::testing::ValuesIn(AllTriggeredBy()));

TEST(RtTriggeredByTokens, ExactStringsAndUniqueness) {
    EXPECT_STREQ(ToString(TriggeredBy::kUserManual), "user_manual");
    EXPECT_STREQ(ToString(TriggeredBy::kUserEdit), "user_edit");
    EXPECT_STREQ(ToString(TriggeredBy::kAdminRevoke), "admin_revoke");
    EXPECT_STREQ(ToString(TriggeredBy::kLlmAuto), "llm_auto");

    std::set<std::string> seen;
    for (TriggeredBy t : AllTriggeredBy()) {
        EXPECT_TRUE(seen.insert(ToString(t)).second);
    }
    EXPECT_EQ(seen.size(), 4u);
}

}  // namespace
}  // namespace cortrix::memory::transparency
