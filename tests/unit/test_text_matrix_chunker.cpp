// Breadth matrix for cortrix::chunker::EstimateTokens (parent-child chunking standalone char-based
// heuristic). Real impl (src/chunker/parent_child_chunker.cpp):
//   - whitespace ' ' '\n' '\t' '\r' '\f' are skipped (token separators, 0 tokens).
//   - an ASCII non-ws run of length L contributes max(1, (L+3)/4) tokens.
//   - each non-ASCII lead byte contributes exactly 1 token and advances by the
//     UTF-8 width (>=0xF0 -> 4, >=0xE0 -> 3, >=0xC0 -> 2, else 1), i.e. one token
//     per UTF-8 *character* for well-formed multibyte (CJK 1 tok/char; emoji 1).
//   - empty / whitespace-only -> 0. Never 0 for real content.
//
// We assert EXACT counts (the heuristic is fully deterministic) plus invariants.
// ASCII-only source; CJK/emoji DATA via \x UTF-8 escapes.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "cortrix/chunker/i_chunker.h"

namespace cortrix::chunker {
namespace {

// CJK chars (3-byte each).
const std::string kHao = "\xE5\xA5\xBD";                 // U+597D CJK (3-byte)
const std::string kNi  = "\xE4\xBD\xA0";                 // U+4F60 CJK (3-byte)
const std::string kEmoji = "\xF0\x9F\x98\x80";           // U+1F600 (4-byte)
const std::string kLatin = "\xC3\xA9";                   // U+00E9 latin (2-byte)
const std::string kBom = "\xEF\xBB\xBF";                 // U+FEFF (3-byte)

struct TokCase {
    std::string name;
    std::string input;
    uint32_t expect;
};

class ChunkerEstimateTokensTextMatrix
    : public ::testing::TestWithParam<TokCase> {};

TEST_P(ChunkerEstimateTokensTextMatrix, ExactCount) {
    const TokCase& tc = GetParam();
    EXPECT_EQ(EstimateTokens(tc.input), tc.expect) << tc.name;
}

std::vector<TokCase> MakeTokCases() {
    std::vector<TokCase> v;

    // --- empty / whitespace-only -> 0 ---
    v.push_back({"empty", "", 0});
    v.push_back({"space", " ", 0});
    v.push_back({"spaces", "      ", 0});
    v.push_back({"tabs", "\t\t", 0});
    v.push_back({"newlines", "\n\n\n", 0});
    v.push_back({"cr", "\r\r", 0});
    v.push_back({"formfeed", "\f", 0});
    v.push_back({"mixed_ws", " \t\n\r\f ", 0});

    // --- ASCII runs: max(1,(L+3)/4) ---
    v.push_back({"ascii_1", "a", 1});             // (1+3)/4=1
    v.push_back({"ascii_3", "abc", 1});           // (3+3)/4=1
    v.push_back({"ascii_4", "abcd", 1});          // (4+3)/4=1
    v.push_back({"ascii_5", "abcde", 2});         // (5+3)/4=2
    v.push_back({"ascii_8", "abcdefgh", 2});      // (8+3)/4=2
    v.push_back({"ascii_9", "abcdefghi", 3});     // (9+3)/4=3
    v.push_back({"ascii_12", std::string(12, 'x'), 3});  // (12+3)/4=3
    v.push_back({"ascii_16", std::string(16, 'x'), 4});  // (16+3)/4=4
    v.push_back({"ascii_100", std::string(100, 'x'), 25});  // (100+3)/4=25

    // --- two ASCII runs separated by ws: per-run min 1 ---
    v.push_back({"two_words", "a b", 2});         // 1 + 1
    v.push_back({"hi_world", "hi world", 2});     // hi(1)+world(2? len5)->1+2=3? see below
    // "world" len5 -> (5+3)/4=2 ; "hi" len2 -> 1. total 3.
    v.back().expect = 3;
    v.push_back({"three_words", "aa bb cc", 3});  // each len2 -> 1 each
    v.push_back({"leading_ws", "   hi", 1});
    v.push_back({"trailing_ws", "hi   ", 1});
    v.push_back({"ws_between_long", "abcdefgh  abcdefgh", 4});  // 2+2

    // --- CJK: 1 token per 3-byte char ---
    v.push_back({"cjk_1", kHao, 1});
    v.push_back({"cjk_2", kHao + kNi, 2});
    v.push_back({"cjk_3", kHao + kNi + kHao, 3});
    v.push_back({"cjk_no_ws_split", kHao + kHao + kHao, 3});  // no separators
    // CJK chars adjacent (no ws) each count 1 (multibyte loop).
    {
        std::string s;
        for (int i = 0; i < 10; ++i) s += kHao;
        v.push_back({"cjk_10", s, 10});
    }

    // --- 4-byte emoji: 1 token each ---
    v.push_back({"emoji_1", kEmoji, 1});
    v.push_back({"emoji_3", kEmoji + kEmoji + kEmoji, 3});

    // --- 2-byte latin: 1 token each ---
    v.push_back({"latin_1", kLatin, 1});
    v.push_back({"latin_3", kLatin + kLatin + kLatin, 3});

    // --- BOM (3-byte) counts as 1 multibyte token ---
    v.push_back({"bom_only", kBom, 1});
    // BOM + ascii word: 1 (bom) + ascii run.
    v.push_back({"bom_word", kBom + "hi", 2});    // 1 + (2->1)

    // --- mixed ASCII + CJK ---
    // ASCII run "hi"=1, then one CJK char=1 => 2.
    v.push_back({"ascii_then_cjk", "hi" + kHao, 2});
    // CJK 1 + ascii "hi" 1 => 2.
    v.push_back({"cjk_then_ascii", kHao + "hi", 2});
    // CJK1 + ws + ascii1 + ws + CJK1 = 3.
    v.push_back({"mixed_spaced", kHao + " hi " + kNi, 3});
    // ascii8 + cjk2 + emoji1 = 2+2+1 = 5.
    v.push_back({"mixed_all", std::string(8, 'a') + kHao + kNi + kEmoji, 5});

    // --- control char (non-ws) inside ASCII run: not a separator ---
    // \x01 is <0x80 and not a ws char -> part of the ASCII run.
    v.push_back({"ascii_with_ctrl", std::string("ab\x01" "cd"), 2});  // len5 ->2
    // NUL inside: also <0x80, non-ws -> counted in run. "a\0b" len3 ->1.
    v.push_back({"ascii_with_nul", std::string("a\0b", 3), 1});

    // --- very long / oversize ---
    v.push_back({"long_ascii_4000", std::string(4000, 'a'), 1000});  // 4003/4=1000
    {
        std::string s;
        for (int i = 0; i < 1000; ++i) s += kHao;
        v.push_back({"long_cjk_1000", s, 1000});
    }

    // --- truncated trailing multibyte: lead byte still +1, advance clamped ---
    // 0xE5 lead with no continuation: tokens+1, adv=3 but i clamps to n.
    v.push_back({"truncated_3byte_lead", "\xE5", 1});
    v.push_back({"truncated_2of3", "\xE5\xA5", 1});
    v.push_back({"ascii_then_trunc", "a\xE5", 2});  // ascii 1 + lead 1

    // --- exhaustive ASCII length sweep 1..40: exact max(1,(L+3)/4) ---
    for (uint32_t L = 1; L <= 40; ++L) {
        const uint32_t expect = std::max<uint32_t>(1, (L + 3) / 4);
        v.push_back({"ascii_len_" + std::to_string(L),
                     std::string(L, 'a'), expect});
    }

    // --- exhaustive CJK count sweep 1..30: exact == count ---
    for (uint32_t k = 1; k <= 30; ++k) {
        std::string s;
        for (uint32_t i = 0; i < k; ++i) s += kHao;
        v.push_back({"cjk_count_" + std::to_string(k), s, k});
    }

    // --- exhaustive emoji count sweep 1..20: exact == count ---
    for (uint32_t k = 1; k <= 20; ++k) {
        std::string s;
        for (uint32_t i = 0; i < k; ++i) s += kEmoji;
        v.push_back({"emoji_count_" + std::to_string(k), s, k});
    }

    return v;
}

INSTANTIATE_TEST_SUITE_P(
    ChunkerEstimateTokensTextMatrixInst, ChunkerEstimateTokensTextMatrix,
    ::testing::ValuesIn(MakeTokCases()),
    [](const ::testing::TestParamInfo<TokCase>& info) {
        return info.param.name;
    });

// Invariants (independent of exact counts).
TEST(ChunkerEstimateTokensTextInvariant, NonWsContentNeverZero) {
    EXPECT_GT(EstimateTokens("x"), 0u);
    EXPECT_GT(EstimateTokens(kHao), 0u);
    EXPECT_GT(EstimateTokens(kEmoji), 0u);
}

TEST(ChunkerEstimateTokensTextInvariant, Deterministic) {
    const std::string s = "hello " + kHao + kEmoji + " world";
    EXPECT_EQ(EstimateTokens(s), EstimateTokens(s));
}

TEST(ChunkerEstimateTokensTextInvariant, WhitespacePaddingDoesNotChangeCount) {
    EXPECT_EQ(EstimateTokens("hello"), EstimateTokens("   hello   "));
    EXPECT_EQ(EstimateTokens(kHao + kNi),
              EstimateTokens(" " + kHao + " " + kNi + " "));
}

}  // namespace
}  // namespace cortrix::chunker
