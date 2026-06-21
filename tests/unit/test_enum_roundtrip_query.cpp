#include <gtest/gtest.h>

#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cortrix/query/query_request.h"
#include "cortrix/query/rag_fusion_types.h"
#include "cortrix/retrieval/sparse_rrf.h"

// Exhaustive matrices for the query/retrieval enums.
//   VariantStrategy (cortrix::query): VariantStrategyString <-> ParseVariantStrategy
//                                     (round-trip) + nlohmann to_json/from_json
//                                     (unknown JSON string -> kParaphrase).
//   RrfPath (cortrix::retrieval):     ToString only (5 paths, F40 GS-1 labels).
//   BlockType (cortrix):              BlockTypeToString <-> BlockTypeFromString
//                                     (int codes 1..7). unknown int -> "UNKNOWN";
//                                     unknown string -> -1.
// Suite names unique with RtQuery* / RtRrf* / RtBlockType* prefixes.

namespace cortrix::query {
namespace {

const std::vector<VariantStrategy>& AllVariantStrategies() {
    static const std::vector<VariantStrategy> v = {
        VariantStrategy::kParaphrase, VariantStrategy::kSubquery,
        VariantStrategy::kReverse};
    return v;
}

class RtVariantStrategyMatrix
    : public ::testing::TestWithParam<VariantStrategy> {};

TEST_P(RtVariantStrategyMatrix, StringParseRoundTrip) {
    const VariantStrategy s = GetParam();
    const std::string token = VariantStrategyString(s);
    EXPECT_FALSE(token.empty());
    VariantStrategy out = VariantStrategy::kParaphrase;
    ASSERT_TRUE(ParseVariantStrategy(token, &out))
        << "token did not parse: " << token;
    EXPECT_EQ(out, s) << "round-trip mismatch for token: " << token;
}

TEST_P(RtVariantStrategyMatrix, JsonRoundTrip) {
    const VariantStrategy s = GetParam();
    nlohmann::json j = s;  // to_json
    ASSERT_TRUE(j.is_string());
    EXPECT_EQ(j.get<std::string>(), std::string(VariantStrategyString(s)));
    VariantStrategy back = j.get<VariantStrategy>();  // from_json
    EXPECT_EQ(back, s);
}

INSTANTIATE_TEST_SUITE_P(AllValues, RtVariantStrategyMatrix,
                         ::testing::ValuesIn(AllVariantStrategies()));

TEST(RtVariantStrategyTokens, ExactStrings) {
    EXPECT_STREQ(VariantStrategyString(VariantStrategy::kParaphrase),
                 "paraphrase");
    EXPECT_STREQ(VariantStrategyString(VariantStrategy::kSubquery), "subquery");
    EXPECT_STREQ(VariantStrategyString(VariantStrategy::kReverse), "reverse");
}

// Unknown string -> ParseVariantStrategy returns false (out untouched); from_json
// deserializes an unknown / non-string to kParaphrase (defensive, never throws).
class RtVariantStrategyUnknownMatrix
    : public ::testing::TestWithParam<std::string> {};

TEST_P(RtVariantStrategyUnknownMatrix, ParseReturnsFalseAndLeavesOut) {
    VariantStrategy out = VariantStrategy::kReverse;  // sentinel != default
    EXPECT_FALSE(ParseVariantStrategy(GetParam(), &out))
        << "unexpectedly parsed: " << GetParam();
    EXPECT_EQ(out, VariantStrategy::kReverse) << "out was mutated on failure";
}

TEST_P(RtVariantStrategyUnknownMatrix, JsonFromUnknownStringIsParaphrase) {
    nlohmann::json j = GetParam();
    EXPECT_EQ(j.get<VariantStrategy>(), VariantStrategy::kParaphrase);
}

INSTANTIATE_TEST_SUITE_P(Unknowns, RtVariantStrategyUnknownMatrix,
                         ::testing::Values(std::string(""), "Paraphrase",
                                           "SUBQUERY", "rewrite", "decompose"));

TEST(RtVariantStrategyJson, NonStringDefaultsToParaphrase) {
    nlohmann::json num = 42;
    EXPECT_EQ(num.get<VariantStrategy>(), VariantStrategy::kParaphrase);
    nlohmann::json arr = nlohmann::json::array();
    EXPECT_EQ(arr.get<VariantStrategy>(), VariantStrategy::kParaphrase);
}

}  // namespace
}  // namespace cortrix::query

// ----------------------------------------------------------------------------
// RrfPath: ToString only (5 paths; cortrix::retrieval). Pin tokens + uniqueness;
// kRrfPathCount must equal the enumerated value count.
// ----------------------------------------------------------------------------
namespace cortrix::retrieval {
namespace {

const std::vector<RrfPath>& AllRrfPaths() {
    static const std::vector<RrfPath> v = {
        RrfPath::kDense, RrfPath::kContextualized, RrfPath::kSparse,
        RrfPath::kFts5, RrfPath::kHypeQuestion};
    return v;
}

class RtRrfPathMatrix : public ::testing::TestWithParam<RrfPath> {};

TEST_P(RtRrfPathMatrix, TokenNonEmptyLowercase) {
    const std::string token = ToString(GetParam());
    EXPECT_FALSE(token.empty());
    for (char c : token) {
        EXPECT_TRUE((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                    c == '_')
            << "unexpected token char in: " << token;
    }
}

INSTANTIATE_TEST_SUITE_P(AllValues, RtRrfPathMatrix,
                         ::testing::ValuesIn(AllRrfPaths()));

TEST(RtRrfPathTokens, ExactStringsCountAndUniqueness) {
    EXPECT_STREQ(ToString(RrfPath::kDense), "dense");
    EXPECT_STREQ(ToString(RrfPath::kContextualized), "contextualized");
    EXPECT_STREQ(ToString(RrfPath::kSparse), "sparse");
    EXPECT_STREQ(ToString(RrfPath::kFts5), "fts5");
    EXPECT_STREQ(ToString(RrfPath::kHypeQuestion), "hype_question");

    EXPECT_EQ(static_cast<int>(AllRrfPaths().size()), kRrfPathCount);

    std::set<std::string> seen;
    for (RrfPath p : AllRrfPaths()) {
        EXPECT_TRUE(seen.insert(ToString(p)).second);
    }
    EXPECT_EQ(static_cast<int>(seen.size()), kRrfPathCount);
}

}  // namespace
}  // namespace cortrix::retrieval

// ----------------------------------------------------------------------------
// BlockType: BlockTypeToString(int) <-> BlockTypeFromString(string), codes 1..7
// (cortrix, query_request.h inline helpers).
// ----------------------------------------------------------------------------
namespace cortrix {
namespace {

class RtBlockTypeMatrix : public ::testing::TestWithParam<int> {};

TEST_P(RtBlockTypeMatrix, IntToStringToIntRoundTrip) {
    const int code = GetParam();
    const std::string token = BlockTypeToString(code);
    EXPECT_NE(token, "UNKNOWN");
    EXPECT_EQ(BlockTypeFromString(token), code)
        << "round-trip mismatch for code: " << code;
}

INSTANTIATE_TEST_SUITE_P(Codes1To7, RtBlockTypeMatrix,
                         ::testing::Values(1, 2, 3, 4, 5, 6, 7));

TEST(RtBlockTypeTokens, ExactStrings) {
    EXPECT_EQ(BlockTypeToString(1), "FILE");
    EXPECT_EQ(BlockTypeToString(2), "SCAN");
    EXPECT_EQ(BlockTypeToString(3), "DATABASE");
    EXPECT_EQ(BlockTypeToString(4), "AUDIO");
    EXPECT_EQ(BlockTypeToString(5), "VIDEO");
    EXPECT_EQ(BlockTypeToString(6), "IMAGE");
    EXPECT_EQ(BlockTypeToString(7), "MEMORY");
}

// Unknown int code -> "UNKNOWN"; unknown string -> -1.
class RtBlockTypeUnknownIntMatrix : public ::testing::TestWithParam<int> {};

TEST_P(RtBlockTypeUnknownIntMatrix, UnknownIntIsUnknownString) {
    EXPECT_EQ(BlockTypeToString(GetParam()), "UNKNOWN");
}

INSTANTIATE_TEST_SUITE_P(BadCodes, RtBlockTypeUnknownIntMatrix,
                         ::testing::Values(0, -1, 8, 99, 100));

class RtBlockTypeUnknownStrMatrix
    : public ::testing::TestWithParam<std::string> {};

TEST_P(RtBlockTypeUnknownStrMatrix, UnknownStringIsMinusOne) {
    EXPECT_EQ(BlockTypeFromString(GetParam()), -1);
}

INSTANTIATE_TEST_SUITE_P(BadStrings, RtBlockTypeUnknownStrMatrix,
                         ::testing::Values(std::string(""), "file", "Text",
                                           "UNKNOWN", "DOC", "memory"));

}  // namespace
}  // namespace cortrix
