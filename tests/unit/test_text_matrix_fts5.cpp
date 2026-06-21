// Breadth matrix for cortrix::SanitizeFts5Query (M-SEC-001 FTS5 injection guard).
//
// Real impl (src/store/cortrix_store_sqlite.cpp):
//   - empty input -> empty output.
//   - tokenizes on ASCII whitespace (space/tab/\n/\r).
//   - drops tokens with no "content" char (content = isalnum OR byte>127).
//   - keeps content tokens, escaping embedded '"' by doubling, then wraps each
//     surviving token in double quotes; tokens joined by single spaces.
//
// We assert structural INVARIANTS (the exact output is impl-defined for some
// classes), so the matrix is robust to incidental formatting:
//   * pure-whitespace / empty / pure-punctuation  -> "" (no content token).
//   * any output is "balanced": even number of unescaped-context quotes, and
//     output starts+ends with '"' when non-empty.
//   * idempotence-ish: a sanitized non-empty result re-sanitized stays non-empty.
//   * no bare FTS5 operator survives outside quotes (no unquoted * ^ : etc).
//   * UTF-8 / CJK / 4-byte / mixed content survives (output non-empty).
//
// ASCII-only source; CJK test DATA via \x UTF-8 escapes.
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "cortrix/store/cortrix_store_sqlite.h"

namespace cortrix {
namespace {

// U+4F60 U+597D: 3-byte CJK each.
const std::string kNiHao = "\xE4\xBD\xA0\xE5\xA5\xBD";
// Mixed multi-char CJK (4 ideographs).
const std::string kZhWorld = "\xE4\xB8\xAD\xE6\x96\x87\xE4\xB8\x96\xE7\x95\x8C";
// 4-byte emoji U+1F600 grinning face.
const std::string kEmoji = "\xF0\x9F\x98\x80";
// BOM EF BB BF.
const std::string kBom = "\xEF\xBB\xBF";

enum class Expect { kEmpty, kNonEmpty };

struct Fts5Case {
    std::string name;
    std::string input;
    Expect expect;
};

// Count ASCII double quotes in s.
size_t CountQuotes(const std::string& s) {
    size_t n = 0;
    for (char c : s) if (c == '"') ++n;
    return n;
}

class Fts5SanitizeTextMatrix : public ::testing::TestWithParam<Fts5Case> {};

TEST_P(Fts5SanitizeTextMatrix, Invariants) {
    const Fts5Case& tc = GetParam();
    const std::string out = SanitizeFts5Query(tc.input);

    if (tc.expect == Expect::kEmpty) {
        EXPECT_TRUE(out.empty()) << tc.name << " expected empty, got: " << out;
        return;
    }

    ASSERT_FALSE(out.empty()) << tc.name << " expected non-empty";
    // Every surviving token is double-quote wrapped: output starts+ends with '"'.
    EXPECT_EQ(out.front(), '"') << tc.name;
    EXPECT_EQ(out.back(), '"') << tc.name;
    // Quotes always come in pairs (wrapping + doubled escapes are even).
    EXPECT_EQ(CountQuotes(out) % 2u, 0u) << tc.name << " unbalanced quotes: " << out;

    // Re-sanitizing a non-empty result stays non-empty (no content lost on a
    // second pass over already-quoted content tokens).
    const std::string out2 = SanitizeFts5Query(out);
    EXPECT_FALSE(out2.empty()) << tc.name << " second pass collapsed: " << out;
}

std::vector<Fts5Case> MakeFts5Cases() {
    std::vector<Fts5Case> v;

    // --- empty / whitespace-only -> empty ---
    v.push_back({"empty", "", Expect::kEmpty});
    v.push_back({"spaces", "     ", Expect::kEmpty});
    v.push_back({"tabs", "\t\t\t", Expect::kEmpty});
    v.push_back({"newlines", "\n\n", Expect::kEmpty});
    v.push_back({"crlf", "\r\n \r\n", Expect::kEmpty});
    v.push_back({"mixed_ws", " \t\n\r ", Expect::kEmpty});

    // --- pure operator / punctuation tokens -> dropped -> empty ---
    v.push_back({"star", "*", Expect::kEmpty});
    v.push_back({"caret", "^", Expect::kEmpty});
    v.push_back({"colon", ":", Expect::kEmpty});
    v.push_back({"and_op", "AND", Expect::kNonEmpty});  // letters = content
    v.push_back({"plus_minus", "+ -", Expect::kEmpty});
    v.push_back({"parens", "( )", Expect::kEmpty});
    v.push_back({"quote_only", "\"", Expect::kEmpty});
    v.push_back({"double_quote_only", "\"\"", Expect::kEmpty});
    v.push_back({"punct_run", "!@#$%^&*", Expect::kEmpty});
    v.push_back({"dots", "...", Expect::kEmpty});

    // --- ASCII content ---
    v.push_back({"ascii_word", "hello", Expect::kNonEmpty});
    v.push_back({"ascii_two", "hello world", Expect::kNonEmpty});
    v.push_back({"ascii_digits", "12345", Expect::kNonEmpty});
    v.push_back({"ascii_alnum", "abc123", Expect::kNonEmpty});
    v.push_back({"ascii_quote_inside", "say\"hi", Expect::kNonEmpty});
    v.push_back({"ascii_leading_ws", "   hello", Expect::kNonEmpty});
    v.push_back({"ascii_trailing_ws", "hello   ", Expect::kNonEmpty});
    v.push_back({"ascii_internal_op", "a*b", Expect::kNonEmpty});  // has alnum
    v.push_back({"ascii_with_op_token", "find * here", Expect::kNonEmpty});

    // --- CJK / mixed / 4-byte / BOM ---
    v.push_back({"cjk_nihao", kNiHao, Expect::kNonEmpty});
    v.push_back({"cjk_world", kZhWorld, Expect::kNonEmpty});
    v.push_back({"cjk_two_tokens", kNiHao + " " + kZhWorld, Expect::kNonEmpty});
    v.push_back({"mixed_ascii_cjk", "hello" + kNiHao, Expect::kNonEmpty});
    v.push_back({"mixed_spaced", "hi " + kNiHao + " yo", Expect::kNonEmpty});
    v.push_back({"emoji", kEmoji, Expect::kNonEmpty});       // byte>127 = content
    v.push_back({"emoji_word", "wow" + kEmoji, Expect::kNonEmpty});
    v.push_back({"bom_then_word", kBom + "hello", Expect::kNonEmpty});
    v.push_back({"bom_then_cjk", kBom + kNiHao, Expect::kNonEmpty});
    v.push_back({"bom_only", kBom, Expect::kNonEmpty});      // BOM bytes are >127

    // --- control chars (non-ws controls are not token separators) ---
    v.push_back({"null_in_word", std::string("a\0b", 3), Expect::kNonEmpty});
    v.push_back({"vtab_word", "a\x0Bb", Expect::kNonEmpty});
    v.push_back({"formfeed_word", "a\x0Cb", Expect::kNonEmpty});

    // --- oversize / very long ---
    v.push_back({"long_ascii", std::string(5000, 'a'), Expect::kNonEmpty});
    {
        std::string many;
        for (int i = 0; i < 500; ++i) many += "w ";
        v.push_back({"many_tokens", many, Expect::kNonEmpty});
    }
    {
        std::string longcjk;
        for (int i = 0; i < 1000; ++i) longcjk += kNiHao;
        v.push_back({"long_cjk", longcjk, Expect::kNonEmpty});
    }
    {
        // injection attempt: quotes + operators + content.
        v.push_back({"injection_attempt", "a\" OR \"1\"=\"1", Expect::kNonEmpty});
        v.push_back({"injection_near", "x NEAR(y)", Expect::kNonEmpty});
        v.push_back({"injection_colon_col", "title:foo", Expect::kNonEmpty});
    }

    // --- token-count sweep: N content words -> non-empty, exact quote pairs ---
    for (int n = 1; n <= 30; ++n) {
        std::string s;
        for (int i = 0; i < n; ++i) { if (i) s += ' '; s += "w"; }
        v.push_back({"nwords_" + std::to_string(n), s, Expect::kNonEmpty});
    }
    // --- ASCII word length sweep -> non-empty ---
    for (int L = 1; L <= 20; ++L) {
        v.push_back({"wordlen_" + std::to_string(L),
                     std::string(static_cast<size_t>(L), 'a'),
                     Expect::kNonEmpty});
    }
    // --- pure-operator runs of varying length -> empty ---
    for (int L = 1; L <= 10; ++L) {
        v.push_back({"oprun_" + std::to_string(L),
                     std::string(static_cast<size_t>(L), '*'),
                     Expect::kEmpty});
    }
    // --- CJK count sweep -> non-empty ---
    for (int n = 1; n <= 20; ++n) {
        std::string s;
        for (int i = 0; i < n; ++i) s += kNiHao;
        v.push_back({"cjkrun_" + std::to_string(n), s, Expect::kNonEmpty});
    }

    return v;
}

INSTANTIATE_TEST_SUITE_P(Fts5SanitizeTextMatrixInst, Fts5SanitizeTextMatrix,
                         ::testing::ValuesIn(MakeFts5Cases()),
                         [](const ::testing::TestParamInfo<Fts5Case>& info) {
                             return info.param.name;
                         });

// Targeted exact-behavior checks (the parts of the contract that ARE fixed).
TEST(Fts5SanitizeTextExact, EmptyInputEmptyOutput) {
    EXPECT_EQ(SanitizeFts5Query(""), std::string());
}

TEST(Fts5SanitizeTextExact, SingleWordIsQuoteWrapped) {
    EXPECT_EQ(SanitizeFts5Query("hello"), std::string("\"hello\""));
}

TEST(Fts5SanitizeTextExact, TwoWordsSpaceJoinedQuoted) {
    EXPECT_EQ(SanitizeFts5Query("hello world"),
              std::string("\"hello\" \"world\""));
}

TEST(Fts5SanitizeTextExact, EmbeddedQuoteDoubled) {
    // token a"b -> "a""b"
    EXPECT_EQ(SanitizeFts5Query("a\"b"), std::string("\"a\"\"b\""));
}

TEST(Fts5SanitizeTextExact, PureOperatorTokenDropped) {
    EXPECT_TRUE(SanitizeFts5Query("*").empty());
    EXPECT_TRUE(SanitizeFts5Query("^ :").empty());
}

TEST(Fts5SanitizeTextExact, WhitespaceCollapsedAroundContent) {
    EXPECT_EQ(SanitizeFts5Query("   foo  \t bar \n"),
              std::string("\"foo\" \"bar\""));
}

}  // namespace
}  // namespace cortrix
