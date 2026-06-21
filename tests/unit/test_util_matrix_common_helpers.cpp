// Exhaustive deterministic matrices for the small pure helpers in src/common and
// src/chunker:
//  - common/json_depth.h: JsonMaxDepth / JsonExceedsMaxDepth (header-only,
//    iterative depth measurement; scalar/empty container = depth 0; {"a":1}=1;
//    {"a":{"b":1}}=2; over-cap returns a value strictly > cap).
//  - chunker EstimateTokens: char-based heuristic. ASCII non-whitespace runs are
//    ceil(run_len/4) (min 1 per run); whitespace separates runs and is free; each
//    non-ASCII UTF-8 lead byte counts as 1 token. Whitespace-only -> 0.
//
// SUITE PREFIX: JsonDepthUtilMatrix* / EstimateTokensUtilMatrix* (globally unique).
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cortrix/chunker/i_chunker.h"
#include "cortrix/common/json_depth.h"

namespace cortrix {
namespace {

using nlohmann::json;

// ================= JsonMaxDepth matrix =================
struct DepthCase {
    std::string label;
    json value;
    int expected_depth;
};

class JsonDepthUtilMatrixDepth : public ::testing::TestWithParam<DepthCase> {};

TEST_P(JsonDepthUtilMatrixDepth, MaxDepth) {
    const DepthCase& c = GetParam();
    // Use a generous cap so the true depth is returned (not the early-out value).
    EXPECT_EQ(JsonMaxDepth(c.value, 1000), c.expected_depth) << c.label;
}

INSTANTIATE_TEST_SUITE_P(
    Cases, JsonDepthUtilMatrixDepth,
    ::testing::Values(
        // Scalars => depth 0.
        DepthCase{"null", json(nullptr), 0},
        DepthCase{"int", json(42), 0},
        DepthCase{"double", json(3.14), 0},
        DepthCase{"bool", json(true), 0},
        DepthCase{"string", json("hello"), 0},
        // Empty containers => depth 0.
        DepthCase{"empty_object", json::object(), 0},
        DepthCase{"empty_array", json::array(), 0},
        // One level.
        DepthCase{"flat_object", json{{"a", 1}, {"b", 2}}, 1},
        DepthCase{"flat_array", json::array({1, 2, 3}), 1},
        DepthCase{"object_with_empty_child",
                  json{{"a", json::object()}}, 1},  // empty child node sits at depth 1
        // Two levels.
        DepthCase{"nested_object", json{{"a", {{"b", 1}}}}, 2},
        DepthCase{"nested_array", json::array({json::array({1})}), 2},
        // Three levels (object).
        DepthCase{"three_object", json{{"a", {{"b", {{"c", 1}}}}}}, 3},
        // Mixed object/array nesting.
        DepthCase{"mixed", json{{"a", json::array({json{{"b", 1}}})}}, 3},
        // Asymmetric: deepest branch wins.
        DepthCase{"asymmetric",
                  json{{"shallow", 1}, {"deep", {{"x", {{"y", 1}}}}}}, 3},
        // Array of scalars deeper via index.
        DepthCase{"array_of_objects",
                  json::array({json{{"a", 1}}, json{{"b", {{"c", 1}}}}}), 3}));

// JsonMaxDepth early-out: when depth exceeds cap, returns a value strictly > cap.
TEST(JsonDepthUtilMatrixCap, ExceedsCapReturnsGreaterThanCap) {
    // Build {"a":{"a":{"a":...}}} 10 deep.
    json deep = 1;
    for (int i = 0; i < 10; ++i) deep = json{{"a", deep}};  // true depth 10
    // With cap 3, result must be > 3 (and is the first depth seen past the cap).
    int d = JsonMaxDepth(deep, 3);
    EXPECT_GT(d, 3);
    // With a generous cap the exact depth is returned.
    EXPECT_EQ(JsonMaxDepth(deep, 100), 10);
}

// JsonExceedsMaxDepth boundary matrix.
struct ExceedCase {
    int build_depth;
    int max_depth;
    bool expect_exceeds;
};

class JsonDepthUtilMatrixExceeds
    : public ::testing::TestWithParam<ExceedCase> {};

TEST_P(JsonDepthUtilMatrixExceeds, Boundary) {
    const ExceedCase& c = GetParam();
    json v = 1;  // depth 0 scalar
    for (int i = 0; i < c.build_depth; ++i) v = json{{"k", v}};
    EXPECT_EQ(JsonExceedsMaxDepth(v, c.max_depth), c.expect_exceeds)
        << "build=" << c.build_depth << " max=" << c.max_depth;
}

INSTANTIATE_TEST_SUITE_P(
    Cases, JsonDepthUtilMatrixExceeds,
    ::testing::Values(
        ExceedCase{0, 0, false},   // scalar depth 0, max 0 -> not exceeding
        ExceedCase{1, 1, false},   // depth 1 == max 1
        ExceedCase{2, 1, true},    // depth 2 > max 1
        ExceedCase{3, 3, false},   // equal
        ExceedCase{4, 3, true},
        ExceedCase{5, 5, false},
        ExceedCase{6, 5, true},
        ExceedCase{64, 64, false},  // at the default cap
        ExceedCase{65, 64, true}));

// ================= EstimateTokens matrix =================
struct TokenCase {
    std::string label;
    std::string text;
    uint32_t expected;
};

class EstimateTokensUtilMatrix : public ::testing::TestWithParam<TokenCase> {};

TEST_P(EstimateTokensUtilMatrix, Estimate) {
    const TokenCase& c = GetParam();
    EXPECT_EQ(cortrix::chunker::EstimateTokens(c.text), c.expected) << c.label;
}

INSTANTIATE_TEST_SUITE_P(
    Cases, EstimateTokensUtilMatrix,
    ::testing::Values(
        // Empty / whitespace-only => 0 tokens.
        TokenCase{"empty", "", 0},
        TokenCase{"single_space", " ", 0},
        TokenCase{"spaces", "     ", 0},
        TokenCase{"tabs_newlines", "\t\n\r\f ", 0},
        // ASCII runs: ceil(len/4), min 1 per run.
        TokenCase{"one_char", "a", 1},          // ceil(1/4)=1
        TokenCase{"four_chars", "abcd", 1},     // ceil(4/4)=1
        TokenCase{"five_chars", "abcde", 2},    // ceil(5/4)=2
        TokenCase{"eight_chars", "abcdefgh", 2},
        TokenCase{"nine_chars", "abcdefghi", 3},
        // Multiple whitespace-separated runs: each run counted separately.
        TokenCase{"two_short_words", "ab cd", 2},      // 1 + 1
        TokenCase{"three_words", "a b c", 3},          // 1+1+1
        TokenCase{"long_then_short", "abcdefgh xy", 3}, // 2 + 1
        TokenCase{"word_run_5_and_5", "abcde fghij", 4}, // 2 + 2
        // Leading/trailing/multiple internal whitespace is free.
        TokenCase{"padded", "  abcd  ", 1},
        TokenCase{"multi_space_between", "ab     cd", 2},
        // Non-ASCII (CJK): each lead byte = 1 token. 3 CJK chars => 3 tokens.
        TokenCase{"cjk_three", "\xE6\x96\x87\xE6\xA1\xA3\xE6\x95\xB0", 3},
        TokenCase{"cjk_one", "\xE6\x96\x87", 1},
        // Mixed ASCII word + CJK: ceil(5/4)=2 for "hello" + 2 CJK = 4.
        TokenCase{"mixed", "hello\xE6\x96\x87\xE6\xA1\xA3", 4},
        // CJK separated by space from ASCII.
        TokenCase{"mixed_spaced", "\xE6\x96\x87 abcd", 2},  // 1 CJK + 1 ascii run
        // Punctuation counts as ASCII run chars (not whitespace).
        TokenCase{"punct", "a,b.c", 2},  // 5 non-ws ASCII chars => ceil(5/4)=2
        TokenCase{"long_word_16", "abcdefghijklmnop", 4}));  // ceil(16/4)=4

}  // namespace
}  // namespace cortrix
