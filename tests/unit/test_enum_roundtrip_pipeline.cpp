#include <gtest/gtest.h>

#include <set>
#include <string>
#include <vector>

#include "cortrix/chunker/chunker_config.h"
#include "cortrix/spc/cleaning_types.h"
#include "cortrix/spc/parser.h"
#include "cortrix/spc_enricher.h"

// Exhaustive matrices for the SPC pipeline enums.
//   ChunkType (cortrix::spc):    ToString <-> ChunkTypeFromString. unknown -> TEXT.
//   ChunkStrategy (cortrix::chunker): ToString <-> ChunkStrategyFromString.
//                                     unknown -> kParentChild.
//   UnitLevel (cortrix::chunker):     ToString <-> UnitLevelFromString.
//                                     unknown -> kParagraph (lock).
//   EnricherType (cortrix::spc): EnricherTypeString <-> ParseEnricherType.
//                                unknown -> kNull (fail-soft).
//   AnomalyReason (cortrix::spc): ToString only (cleaning, 5 values starting at 1).
// Suite names unique with RtPipe* prefix.

namespace cortrix::spc {
namespace {

// ----------------------------------------------------------------------------
// ChunkType round-trip (5 values, uppercase JSON protocol tokens).
// ----------------------------------------------------------------------------
const std::vector<ChunkType>& AllChunkTypes() {
    static const std::vector<ChunkType> v = {
        ChunkType::TEXT, ChunkType::TABLE, ChunkType::IMAGE_CAPTION,
        ChunkType::CODE, ChunkType::META};
    return v;
}

class RtChunkTypeMatrix : public ::testing::TestWithParam<ChunkType> {};

TEST_P(RtChunkTypeMatrix, ToStringFromStringRoundTrip) {
    const ChunkType type = GetParam();
    const std::string token = ToString(type);
    EXPECT_FALSE(token.empty());
    EXPECT_EQ(ChunkTypeFromString(token), type)
        << "round-trip mismatch for token: " << token;
}

TEST_P(RtChunkTypeMatrix, TokenUppercaseAscii) {
    const std::string token = ToString(GetParam());
    for (char c : token) {
        EXPECT_TRUE((c >= 'A' && c <= 'Z') || c == '_')
            << "non-uppercase token char in: " << token;
    }
}

INSTANTIATE_TEST_SUITE_P(AllValues, RtChunkTypeMatrix,
                         ::testing::ValuesIn(AllChunkTypes()));

TEST(RtChunkTypeTokens, ExactStrings) {
    EXPECT_STREQ(ToString(ChunkType::TEXT), "TEXT");
    EXPECT_STREQ(ToString(ChunkType::TABLE), "TABLE");
    EXPECT_STREQ(ToString(ChunkType::IMAGE_CAPTION), "IMAGE_CAPTION");
    EXPECT_STREQ(ToString(ChunkType::CODE), "CODE");
    EXPECT_STREQ(ToString(ChunkType::META), "META");
}

// Unknown / wrong-case -> TEXT (documented fallback for bridge JSON parsing).
class RtChunkTypeUnknownMatrix : public ::testing::TestWithParam<std::string> {};

TEST_P(RtChunkTypeUnknownMatrix, UnknownDefaultsToText) {
    EXPECT_EQ(ChunkTypeFromString(GetParam()), ChunkType::TEXT)
        << "unexpected non-default for: " << GetParam();
}

INSTANTIATE_TEST_SUITE_P(Unknowns, RtChunkTypeUnknownMatrix,
                         ::testing::Values(std::string(""), "text", "Table",
                                           "FIGURE", "image_caption", "JSON"));

// ----------------------------------------------------------------------------
// EnricherType round-trip (3 values; cortrix::spc). EnricherTypeString /
// ParseEnricherType. unknown -> kNull (fail-soft default path).
// ----------------------------------------------------------------------------
const std::vector<EnricherType>& AllEnricherTypes() {
    static const std::vector<EnricherType> v = {
        EnricherType::kNull, EnricherType::kLlm, EnricherType::kLocalNer};
    return v;
}

class RtEnricherTypeMatrix : public ::testing::TestWithParam<EnricherType> {};

TEST_P(RtEnricherTypeMatrix, StringParseRoundTrip) {
    const EnricherType t = GetParam();
    const std::string token = EnricherTypeString(t);
    EXPECT_FALSE(token.empty());
    EXPECT_EQ(ParseEnricherType(token), t)
        << "round-trip mismatch for token: " << token;
}

INSTANTIATE_TEST_SUITE_P(AllValues, RtEnricherTypeMatrix,
                         ::testing::ValuesIn(AllEnricherTypes()));

TEST(RtEnricherTypeTokens, ExactStrings) {
    EXPECT_STREQ(EnricherTypeString(EnricherType::kNull), "null");
    EXPECT_STREQ(EnricherTypeString(EnricherType::kLlm), "llm");
    EXPECT_STREQ(EnricherTypeString(EnricherType::kLocalNer), "local_ner");
}

class RtEnricherTypeUnknownMatrix
    : public ::testing::TestWithParam<std::string> {};

TEST_P(RtEnricherTypeUnknownMatrix, UnknownDefaultsToNull) {
    EXPECT_EQ(ParseEnricherType(GetParam()), EnricherType::kNull)
        << "unexpected non-default for: " << GetParam();
}

INSTANTIATE_TEST_SUITE_P(Unknowns, RtEnricherTypeUnknownMatrix,
                         ::testing::Values(std::string(""), "NULL", "Llm",
                                           "ner", "gpt", "local-ner"));

// ----------------------------------------------------------------------------
// AnomalyReason: ToString only (cleaning, 5 values, underlying values start at 1).
// ----------------------------------------------------------------------------
const std::vector<AnomalyReason>& AllAnomalyReasons() {
    static const std::vector<AnomalyReason> v = {
        AnomalyReason::EMPTY_CHUNK, AnomalyReason::OVERSIZED_CHUNK,
        AnomalyReason::PARSE_FAILED, AnomalyReason::DUPLICATE_BLOCK_ID,
        AnomalyReason::INVALID_EMBEDDING};
    return v;
}

class RtAnomalyReasonMatrix : public ::testing::TestWithParam<AnomalyReason> {};

TEST_P(RtAnomalyReasonMatrix, TokenNonEmptyLowercase) {
    const std::string token = ToString(GetParam());
    EXPECT_FALSE(token.empty());
    for (char c : token) {
        EXPECT_TRUE((c >= 'a' && c <= 'z') || c == '_')
            << "non-lowercase token char in: " << token;
    }
}

INSTANTIATE_TEST_SUITE_P(AllValues, RtAnomalyReasonMatrix,
                         ::testing::ValuesIn(AllAnomalyReasons()));

TEST(RtAnomalyReasonTokens, ExactStringsAndUniqueness) {
    EXPECT_STREQ(ToString(AnomalyReason::EMPTY_CHUNK), "empty");
    EXPECT_STREQ(ToString(AnomalyReason::OVERSIZED_CHUNK), "oversized");
    EXPECT_STREQ(ToString(AnomalyReason::PARSE_FAILED), "parse_failed");
    EXPECT_STREQ(ToString(AnomalyReason::DUPLICATE_BLOCK_ID), "duplicate_id");
    EXPECT_STREQ(ToString(AnomalyReason::INVALID_EMBEDDING),
                 "invalid_embedding");

    std::set<std::string> seen;
    for (AnomalyReason r : AllAnomalyReasons()) {
        EXPECT_TRUE(seen.insert(ToString(r)).second);
    }
    EXPECT_EQ(seen.size(), 5u);
}

}  // namespace
}  // namespace cortrix::spc

// ----------------------------------------------------------------------------
// ChunkStrategy + UnitLevel round-trip (cortrix::chunker).
// ----------------------------------------------------------------------------
namespace cortrix::chunker {
namespace {

const std::vector<ChunkStrategy>& AllChunkStrategies() {
    static const std::vector<ChunkStrategy> v = {
        ChunkStrategy::kParentChild, ChunkStrategy::kFlat,
        ChunkStrategy::kSemantic};
    return v;
}

class RtChunkStrategyMatrix : public ::testing::TestWithParam<ChunkStrategy> {};

TEST_P(RtChunkStrategyMatrix, ToStringFromStringRoundTrip) {
    const ChunkStrategy s = GetParam();
    const std::string token = ToString(s);
    EXPECT_FALSE(token.empty());
    EXPECT_EQ(ChunkStrategyFromString(token), s)
        << "round-trip mismatch for token: " << token;
}

INSTANTIATE_TEST_SUITE_P(AllValues, RtChunkStrategyMatrix,
                         ::testing::ValuesIn(AllChunkStrategies()));

TEST(RtChunkStrategyTokens, ExactStrings) {
    EXPECT_STREQ(ToString(ChunkStrategy::kParentChild), "parent-child");
    EXPECT_STREQ(ToString(ChunkStrategy::kFlat), "flat");
    EXPECT_STREQ(ToString(ChunkStrategy::kSemantic), "semantic");
}

class RtChunkStrategyUnknownMatrix
    : public ::testing::TestWithParam<std::string> {};

TEST_P(RtChunkStrategyUnknownMatrix, UnknownDefaultsToParentChild) {
    EXPECT_EQ(ChunkStrategyFromString(GetParam()),
              ChunkStrategy::kParentChild)
        << "unexpected non-default for: " << GetParam();
}

INSTANTIATE_TEST_SUITE_P(Unknowns, RtChunkStrategyUnknownMatrix,
                         ::testing::Values(std::string(""), "parentchild",
                                           "Flat", "FLAT", "hierarchical"));

const std::vector<UnitLevel>& AllUnitLevels() {
    static const std::vector<UnitLevel> v = {
        UnitLevel::kPage, UnitLevel::kParagraph, UnitLevel::kSentence};
    return v;
}

class RtUnitLevelMatrix : public ::testing::TestWithParam<UnitLevel> {};

TEST_P(RtUnitLevelMatrix, ToStringFromStringRoundTrip) {
    const UnitLevel u = GetParam();
    const std::string token = ToString(u);
    EXPECT_FALSE(token.empty());
    EXPECT_EQ(UnitLevelFromString(token), u)
        << "round-trip mismatch for token: " << token;
}

INSTANTIATE_TEST_SUITE_P(AllValues, RtUnitLevelMatrix,
                         ::testing::ValuesIn(AllUnitLevels()));

TEST(RtUnitLevelTokens, ExactStrings) {
    EXPECT_STREQ(ToString(UnitLevel::kPage), "page");
    EXPECT_STREQ(ToString(UnitLevel::kParagraph), "paragraph");
    EXPECT_STREQ(ToString(UnitLevel::kSentence), "sentence");
}

class RtUnitLevelUnknownMatrix : public ::testing::TestWithParam<std::string> {};

TEST_P(RtUnitLevelUnknownMatrix, UnknownDefaultsToParagraph) {
    EXPECT_EQ(UnitLevelFromString(GetParam()), UnitLevel::kParagraph)
        << "unexpected non-default for: " << GetParam();
}

INSTANTIATE_TEST_SUITE_P(Unknowns, RtUnitLevelUnknownMatrix,
                         ::testing::Values(std::string(""), "Page", "PAGE",
                                           "word", "token", "line"));

}  // namespace
}  // namespace cortrix::chunker
