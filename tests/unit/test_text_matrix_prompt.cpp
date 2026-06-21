// Breadth matrix for cortrix::memory prompt_templates text helpers (D10):
//   - ContainsCjk(text): true iff any UTF-8 3-byte sequence decodes to
//     U+4E00..U+9FFF (CJK Unified Ideographs). 2-byte/4-byte/ASCII never match.
//   - RenderPrompt(tmpl, token, value): single-pass replace ALL occurrences of
//     token; empty token returns tmpl unchanged.
//   - ExtractPromptTemplate / ContradictionPromptTemplate: kAuto picks ZH iff
//     ContainsCjk(sample), else EN; kZh/kEn force the variant.
//
// ASCII-only source; CJK DATA via \x UTF-8 escapes. Boundary code points checked
// at U+4E00, U+9FFF (in-range) and U+4DFF, U+A000 (out-of-range).
#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "cortrix/memory/prompt_templates.h"

namespace cortrix::memory {
namespace {

// --- UTF-8 boundary encodings (computed by hand, verified by structure) ---
// U+4E00 -> E4 B8 80  (first CJK ideograph)
const std::string kU4E00 = "\xE4\xB8\x80";
// U+9FFF -> E9 BF BF  (last in the block tested by impl, cp<=0x9FFF)
const std::string kU9FFF = "\xE9\xBF\xBF";
// U+4DFF -> E4 B7 BF  (just below the block: NOT CJK)
const std::string kU4DFF = "\xE4\xB7\xBF";
// U+A000 -> EA 80 80  (just above: NOT CJK, Yi syllable block)
const std::string kUA000 = "\xEA\x80\x80";
// U+00E9 latin small e acute -> C3 A9  (2-byte, never CJK)
const std::string kLatinE = "\xC3\xA9";
// U+1F600 emoji -> F0 9F 98 80 (4-byte, never CJK by this impl)
const std::string kEmoji = "\xF0\x9F\x98\x80";
// A mid-block ideograph U+597D -> E5 A5 BD.
const std::string kHao = "\xE5\xA5\xBD";

// ===== ContainsCjk matrix =====
struct CjkCase {
    std::string name;
    std::string input;
    bool expect;
};

// Encode a code point in the BMP (<= 0xFFFF) to UTF-8 (1/2/3-byte forms).
std::string Utf8(uint32_t cp) {
    std::string s;
    if (cp < 0x80) {
        s += static_cast<char>(cp);
    } else if (cp < 0x800) {
        s += static_cast<char>(0xC0 | (cp >> 6));
        s += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        s += static_cast<char>(0xE0 | (cp >> 12));
        s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        s += static_cast<char>(0x80 | (cp & 0x3F));
    }
    return s;
}

class PromptCjkTextMatrix : public ::testing::TestWithParam<CjkCase> {};

TEST_P(PromptCjkTextMatrix, MatchesExpected) {
    const CjkCase& tc = GetParam();
    EXPECT_EQ(ContainsCjk(tc.input), tc.expect) << tc.name;
}

std::vector<CjkCase> MakeCjkCases() {
    std::vector<CjkCase> v;
    // empty / whitespace / ASCII -> false
    v.push_back({"empty", "", false});
    v.push_back({"spaces", "   ", false});
    v.push_back({"ascii_word", "hello", false});
    v.push_back({"ascii_punct", "!@#$%^&*()", false});
    v.push_back({"ascii_digits", "1234567890", false});
    v.push_back({"newlines", "\n\t\r", false});
    // 2-byte latin -> false
    v.push_back({"latin_accent", kLatinE, false});
    v.push_back({"latin_in_ascii", "caf" + kLatinE, false});
    // 4-byte emoji -> false (impl only checks 3-byte block)
    v.push_back({"emoji", kEmoji, false});
    v.push_back({"emoji_with_ascii", "hi" + kEmoji, false});
    // boundary: in-range true
    v.push_back({"cjk_first_U4E00", kU4E00, true});
    v.push_back({"cjk_last_U9FFF", kU9FFF, true});
    v.push_back({"cjk_mid_hao", kHao, true});
    // boundary: out-of-range false
    v.push_back({"below_block_U4DFF", kU4DFF, false});
    v.push_back({"above_block_UA000", kUA000, false});
    // mixed
    v.push_back({"ascii_then_cjk", "hello" + kHao, true});
    v.push_back({"cjk_then_ascii", kHao + "world", true});
    v.push_back({"cjk_amid_emoji", kEmoji + kHao + kEmoji, true});
    v.push_back({"latin_then_cjk", kLatinE + kU4E00, true});
    // trailing truncated 3-byte lead (i+2 >= n guard) -> false, no crash
    v.push_back({"truncated_lead_only", "\xE4", false});
    v.push_back({"truncated_two_bytes", "\xE4\xB8", false});
    v.push_back({"truncated_after_ascii", "a\xE4\xB8", false});
    // BOM EF BB BF decodes to U+FEFF (3-byte, above block) -> false
    v.push_back({"bom", "\xEF\xBB\xBF", false});
    v.push_back({"bom_then_cjk", std::string("\xEF\xBB\xBF") + kU4E00, true});
    // null byte / control surrounded
    v.push_back({"null_then_cjk", std::string("\0", 1) + kHao, true});
    // long ascii -> false
    v.push_back({"long_ascii", std::string(3000, 'x'), false});
    // long cjk -> true
    {
        std::string longcjk;
        for (int i = 0; i < 500; ++i) longcjk += kHao;
        v.push_back({"long_cjk", longcjk, true});
    }
    // cjk at very end of long ascii (scan reaches end)
    v.push_back({"ascii_tail_cjk", std::string(2000, 'a') + kU4E00, true});

    // --- code-point boundary sweep around the block edges ---
    // Just below U+4E00 (false), at/just above (true).
    for (uint32_t cp = 0x4DF0; cp <= 0x4E0F; ++cp) {
        const bool in = (cp >= 0x4E00 && cp <= 0x9FFF);
        v.push_back({"cp_lo_" + std::to_string(cp), Utf8(cp), in});
    }
    // Around the upper edge U+9FFF.
    for (uint32_t cp = 0x9FF0; cp <= 0xA00F; ++cp) {
        const bool in = (cp >= 0x4E00 && cp <= 0x9FFF);
        v.push_back({"cp_hi_" + std::to_string(cp), Utf8(cp), in});
    }
    // A spread of in-block points (all true).
    for (uint32_t cp = 0x5000; cp <= 0x9000; cp += 0x800) {
        v.push_back({"cp_in_" + std::to_string(cp), Utf8(cp), true});
    }
    // 2-byte points (always false: impl ignores 2-byte).
    for (uint32_t cp = 0x80; cp <= 0x780; cp += 0x100) {
        v.push_back({"cp_2byte_" + std::to_string(cp), Utf8(cp), false});
    }
    return v;
}

INSTANTIATE_TEST_SUITE_P(PromptCjkTextMatrixInst, PromptCjkTextMatrix,
                         ::testing::ValuesIn(MakeCjkCases()),
                         [](const ::testing::TestParamInfo<CjkCase>& info) {
                             return info.param.name;
                         });

// ===== RenderPrompt matrix =====
struct RenderCase {
    std::string name;
    std::string tmpl;
    std::string token;
    std::string value;
    std::string expect;
};

class PromptRenderTextMatrix : public ::testing::TestWithParam<RenderCase> {};

TEST_P(PromptRenderTextMatrix, MatchesExpected) {
    const RenderCase& tc = GetParam();
    EXPECT_EQ(RenderPrompt(tc.tmpl, tc.token, tc.value), tc.expect) << tc.name;
}

std::vector<RenderCase> MakeRenderCases() {
    std::vector<RenderCase> v;
    // empty token -> tmpl unchanged (guard).
    v.push_back({"empty_token", "abc{x}def", "", "Z", "abc{x}def"});
    v.push_back({"empty_token_empty_tmpl", "", "", "Z", ""});
    // token absent -> unchanged.
    v.push_back({"token_absent", "no placeholder here", "{x}", "VAL",
                 "no placeholder here"});
    // single occurrence.
    v.push_back({"single", "a{x}b", "{x}", "VAL", "aVALb"});
    // repeated occurrences -> all replaced.
    v.push_back({"repeated", "{x}-{x}-{x}", "{x}", "Y", "Y-Y-Y"});
    // adjacent occurrences.
    v.push_back({"adjacent", "{x}{x}", "{x}", "AB", "ABAB"});
    // empty value -> token removed.
    v.push_back({"empty_value", "a{x}b", "{x}", "", "ab"});
    // empty tmpl, non-empty token -> empty.
    v.push_back({"empty_tmpl", "", "{x}", "V", ""});
    // value contains the token text -> single pass, no re-scan (pos advances).
    v.push_back({"value_contains_token", "a{x}b", "{x}", "{x}", "a{x}b"});
    v.push_back({"value_contains_token_rep", "{x}{x}", "{x}", "{x}q",
                 "{x}q{x}q"});
    // token at start / end.
    v.push_back({"token_at_start", "{x}tail", "{x}", "H", "Htail"});
    v.push_back({"token_at_end", "head{x}", "{x}", "T", "headT"});
    // real placeholder constant.
    v.push_back({"window_placeholder",
                 std::string("ctx: ") + kPlaceholderInteractionWindow + " end",
                 kPlaceholderInteractionWindow, "WINDOW", "ctx: WINDOW end"});
    // CJK value injection.
    v.push_back({"cjk_value", "[{x}]", "{x}", kHao, "[" + kHao + "]"});
    // CJK template + ascii token.
    v.push_back({"cjk_tmpl", kHao + "{x}" + kHao, "{x}", "M",
                 kHao + "M" + kHao});
    // emoji value.
    v.push_back({"emoji_value", "{x}", "{x}", kEmoji, kEmoji});
    // multi-char token.
    v.push_back({"multichar_token", "aaXYaa", "XY", "..", "aa..aa"});
    // overlapping-ish token (non-overlapping single pass).
    v.push_back({"aaa_token_aa", "aaaa", "aa", "b", "bb"});
    // long value.
    v.push_back({"long_value", "{x}", "{x}", std::string(1000, 'z'),
                 std::string(1000, 'z')});

    // --- repeat-count sweep: N copies of token all replaced ---
    for (int n = 1; n <= 25; ++n) {
        std::string tmpl, expect;
        for (int i = 0; i < n; ++i) {
            tmpl += "{x}";
            expect += "QQ";
        }
        v.push_back({"repeat_" + std::to_string(n), tmpl, "{x}", "QQ", expect});
    }
    // --- token-absent length sweep: unchanged ---
    for (int n = 0; n <= 10; ++n) {
        const std::string body(static_cast<size_t>(n) * 3, 'a');
        v.push_back({"absent_len_" + std::to_string(n), body, "{zzz}", "V",
                     body});
    }
    return v;
}

INSTANTIATE_TEST_SUITE_P(PromptRenderTextMatrixInst, PromptRenderTextMatrix,
                         ::testing::ValuesIn(MakeRenderCases()),
                         [](const ::testing::TestParamInfo<RenderCase>& info) {
                             return info.param.name;
                         });

// ===== Template selector matrix (kAuto resolves via ContainsCjk) =====
struct SelectCase {
    std::string name;
    PromptLang lang;
    std::string sample;
    bool expect_zh;  // true => returns the ZH constant
};

class PromptSelectTextMatrix : public ::testing::TestWithParam<SelectCase> {};

TEST_P(PromptSelectTextMatrix, ExtractAndContradictionAgree) {
    const SelectCase& tc = GetParam();
    const char* ext = ExtractPromptTemplate(tc.lang, tc.sample);
    const char* con = ContradictionPromptTemplate(tc.lang, tc.sample);
    ASSERT_NE(ext, nullptr) << tc.name;
    ASSERT_NE(con, nullptr) << tc.name;
    if (tc.expect_zh) {
        EXPECT_EQ(ext, kMemoryExtractPromptZh) << tc.name;
        EXPECT_EQ(con, kContradictionDetectPromptZh) << tc.name;
    } else {
        EXPECT_EQ(ext, kMemoryExtractPromptEn) << tc.name;
        EXPECT_EQ(con, kContradictionDetectPromptEn) << tc.name;
    }
}

std::vector<SelectCase> MakeSelectCases() {
    std::vector<SelectCase> v;
    // forced variants ignore sample.
    v.push_back({"force_zh_ascii", PromptLang::kZh, "hello", true});
    v.push_back({"force_zh_empty", PromptLang::kZh, "", true});
    v.push_back({"force_en_cjk", PromptLang::kEn, kHao, false});
    v.push_back({"force_en_empty", PromptLang::kEn, "", false});
    // auto: ASCII -> EN.
    v.push_back({"auto_ascii", PromptLang::kAuto, "hello world", false});
    v.push_back({"auto_empty", PromptLang::kAuto, "", false});
    v.push_back({"auto_emoji", PromptLang::kAuto, kEmoji, false});
    v.push_back({"auto_latin", PromptLang::kAuto, kLatinE, false});
    // auto: CJK -> ZH.
    v.push_back({"auto_cjk", PromptLang::kAuto, kHao, true});
    v.push_back({"auto_mixed", PromptLang::kAuto, "hi" + kHao, true});
    v.push_back({"auto_cjk_boundary_first", PromptLang::kAuto, kU4E00, true});
    v.push_back({"auto_cjk_boundary_last", PromptLang::kAuto, kU9FFF, true});
    v.push_back({"auto_below_block", PromptLang::kAuto, kU4DFF, false});
    return v;
}

INSTANTIATE_TEST_SUITE_P(PromptSelectTextMatrixInst, PromptSelectTextMatrix,
                         ::testing::ValuesIn(MakeSelectCases()),
                         [](const ::testing::TestParamInfo<SelectCase>& info) {
                             return info.param.name;
                         });

// Constants integrity (exact, fixed contract).
TEST(PromptConstantsText, ExtractCarryWindowPlaceholder) {
    EXPECT_NE(std::string(kMemoryExtractPromptZh).find(kPlaceholderInteractionWindow),
              std::string::npos);
    EXPECT_NE(std::string(kMemoryExtractPromptEn).find(kPlaceholderInteractionWindow),
              std::string::npos);
}

TEST(PromptConstantsText, ContradictionCarryFactPlaceholders) {
    const std::string zh(kContradictionDetectPromptZh);
    const std::string en(kContradictionDetectPromptEn);
    EXPECT_NE(zh.find(kPlaceholderNewFact), std::string::npos);
    EXPECT_NE(zh.find(kPlaceholderOldFact), std::string::npos);
    EXPECT_NE(en.find(kPlaceholderNewFact), std::string::npos);
    EXPECT_NE(en.find(kPlaceholderOldFact), std::string::npos);
}

}  // namespace
}  // namespace cortrix::memory
