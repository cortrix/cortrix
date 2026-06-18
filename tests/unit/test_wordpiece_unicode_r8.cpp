// R8 systematic Unicode branch coverage for src/query/wordpiece_tokenizer.cpp
// (the #1 R7-BRANCH-GAP target: 223/580 missed @ 61.6%). The missed branches are
// the code-point RANGE arms in the normalizer/classifier helpers:
//   - StripLatinAccent : the Latin-1 (U+00C0–00FF) + Latin Extended-A (U+0100–017F)
//                        accent-fold tables (each case-label group is a branch);
//   - ToLower          : the ASCII A-Z + Latin-1 uppercase (U+00C0–00DE, ≠ ×) arms;
//   - StripAccents     : the IsCombiningMark drop;
//   - IsChineseChar    : the 8 CJK ideograph block ranges;
//   - IsPunctuation    : the ASCII punctuation sub-ranges + the Unicode punctuation
//                        blocks (general punct, CJK punct, fullwidth);
//   - IsControl        : C0 / DEL / C1 + the format-char (Cf) set;
//   - IsWhitespace     : the Unicode space-separator set.
//
// These helpers are PRIVATE static (wordpiece_tokenizer.h:95-120), so the tests
// drive them through the PUBLIC BasicTokenize() — its normalizer chain is
// CleanText → PadChineseChars → ToLower → StripAccents, then the pre-tokenizer
// splits on whitespace + punctuation. Each case asserts the OBSERVABLE result that
// can only occur if the targeted range arm fired (e.g. "À" → "a" proves both the
// ToLower Latin-1 arm and the StripLatinAccent 0x00E0-group; a CJK char emitted as
// its own 1-char token proves the IsChineseChar block; a control char vanishing
// proves the IsControl arm). Data-driven so one case row = one or more range arms.
//
// Standalone NEW file; touches no existing test. (R7 already covered the
// BasicTokenize structural boundary arms + the LoadVocab/Encode guards in
// test_spc_branches_r7b.cpp; this file is purely the Unicode range arms.)
#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "cortrix/query/wordpiece_tokenizer.h"

namespace cortrix::query {
namespace {

// Encode a single Unicode code point as UTF-8 (mirrors the tokenizer's own
// AppendUtf8, but local so the test is self-contained). Lets a case row name a
// raw code point and feed its real multi-byte form through the public API.
std::string U8(char32_t cp) {
    std::string out;
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
    return out;
}

std::string U8Seq(const std::vector<char32_t>& cps) {
    std::string out;
    for (char32_t cp : cps) out += U8(cp);
    return out;
}

// =====================================================================
// StripLatinAccent + ToLower: precomposed accented Latin → base lowercase.
// BasicTokenize lowercases THEN strips accents, so "À"(U+00C0) → "a", etc.
// One row per accent-table group hits that group's case-label branch (and the
// ToLower Latin-1 arm for the uppercase forms).
// =====================================================================

struct AccentCase {
    char32_t cp;            // input code point (single char)
    const char* expected;   // expected single normalized token
};

class AccentFoldTest : public ::testing::TestWithParam<AccentCase> {};

TEST_P(AccentFoldTest, FoldsToBaseLowercase) {
    const auto& c = GetParam();
    WordPieceTokenizer t;
    auto words = t.BasicTokenize(U8(c.cp));
    ASSERT_EQ(words.size(), 1u) << "cp=" << std::hex << static_cast<uint32_t>(c.cp);
    EXPECT_EQ(words[0], c.expected) << "cp=" << std::hex << static_cast<uint32_t>(c.cp);
}

INSTANTIATE_TEST_SUITE_P(Latin1Upper, AccentFoldTest, ::testing::Values(
    // U+00C0–00C5 À Á Â Ã Ä Å → a (ToLower → 00E0-group, StripLatinAccent → 'a')
    AccentCase{0x00C0, "a"}, AccentCase{0x00C3, "a"}, AccentCase{0x00C5, "a"},
    AccentCase{0x00C7, "c"},                                   // Ç
    AccentCase{0x00C8, "e"}, AccentCase{0x00CB, "e"},          // È Ë
    AccentCase{0x00CC, "i"}, AccentCase{0x00CF, "i"},          // Ì Ï
    AccentCase{0x00D1, "n"},                                   // Ñ
    AccentCase{0x00D2, "o"}, AccentCase{0x00D6, "o"},          // Ò Ö
    AccentCase{0x00D9, "u"}, AccentCase{0x00DC, "u"},          // Ù Ü
    AccentCase{0x00DD, "y"}                                    // Ý
));

INSTANTIATE_TEST_SUITE_P(Latin1Lower, AccentFoldTest, ::testing::Values(
    // U+00E0–00FF lowercase accented forms (StripLatinAccent lower arms directly).
    AccentCase{0x00E0, "a"}, AccentCase{0x00E5, "a"},          // à å
    AccentCase{0x00E7, "c"},                                   // ç
    AccentCase{0x00E8, "e"}, AccentCase{0x00EB, "e"},          // è ë
    AccentCase{0x00EC, "i"}, AccentCase{0x00EF, "i"},          // ì ï
    AccentCase{0x00F1, "n"},                                   // ñ
    AccentCase{0x00F2, "o"}, AccentCase{0x00F6, "o"},          // ò ö
    AccentCase{0x00F9, "u"}, AccentCase{0x00FC, "u"},          // ù ü
    AccentCase{0x00FD, "y"}, AccentCase{0x00FF, "y"}           // ý ÿ
));

INSTANTIATE_TEST_SUITE_P(LatinExtendedA, AccentFoldTest, ::testing::Values(
    // U+0100–017F: the StripLatinAccent second switch. The pipeline is uncased —
    // ToLower now lowercases the whole Extended-A block (HF Unicode-aware lowercase)
    // BEFORE StripAccents — so every Extended-A letter, upper- or lower-case, folds
    // to the LOWERCASE base letter (R9 wordpiece fidelity fix; matches HF
    // BertNormalizer ground truth).
    AccentCase{0x0101, "a"}, AccentCase{0x0103, "a"}, AccentCase{0x0105, "a"},  // ā ă ą
    AccentCase{0x0107, "c"}, AccentCase{0x010D, "c"},                            // ć č
    AccentCase{0x0113, "e"}, AccentCase{0x011B, "e"},                            // ē ě
    AccentCase{0x011F, "g"}, AccentCase{0x0121, "g"},                            // ğ ġ
    AccentCase{0x0129, "i"}, AccentCase{0x012B, "i"},                            // ĩ ī
    // ł (U+0142): the stroke is NOT a combining mark, so HF keeps it ł (not "l").
    AccentCase{0x0142, "ł"},                                                     // ł → ł
    AccentCase{0x0144, "n"}, AccentCase{0x0148, "n"},                            // ń ň
    AccentCase{0x014D, "o"}, AccentCase{0x0151, "o"},                            // ō ő
    AccentCase{0x0155, "r"}, AccentCase{0x0159, "r"},                            // ŕ ř
    AccentCase{0x015B, "s"}, AccentCase{0x0161, "s"},                            // ś š
    AccentCase{0x0163, "t"}, AccentCase{0x0165, "t"},                            // ţ ť
    AccentCase{0x0169, "u"}, AccentCase{0x0173, "u"},                            // ũ ų
    AccentCase{0x017A, "z"}, AccentCase{0x017E, "z"}                             // ź ž
));

// R9 wordpiece fidelity fix — the UPPERCASE Latin Extended-A letters now fold to the
// LOWERCASE base (ToLower lowercases the block before StripAccents, matching HF).
// Before the fix these stayed uppercase (Ā→"A"); the idempotence property test
// (test_wordpiece_property_r9.cpp) surfaced the divergence. Uppercase Ł folds to ł.
INSTANTIATE_TEST_SUITE_P(LatinExtendedAUpper, AccentFoldTest, ::testing::Values(
    AccentCase{0x0100, "a"}, AccentCase{0x0104, "a"},                            // Ā Ą
    AccentCase{0x0106, "c"}, AccentCase{0x010C, "c"},                            // Ć Č
    AccentCase{0x0112, "e"}, AccentCase{0x011A, "e"},                            // Ē Ě
    AccentCase{0x011E, "g"},                                                     // Ğ
    AccentCase{0x0128, "i"}, AccentCase{0x012A, "i"},                            // Ĩ Ī
    AccentCase{0x0141, "ł"},                                                     // Ł → ł (lowercased, stroke kept)
    AccentCase{0x0143, "n"}, AccentCase{0x0147, "n"},                            // Ń Ň
    AccentCase{0x014C, "o"}, AccentCase{0x0150, "o"},                            // Ō Ő
    AccentCase{0x0154, "r"}, AccentCase{0x0158, "r"},                            // Ŕ Ř
    AccentCase{0x015A, "s"}, AccentCase{0x0160, "s"},                            // Ś Š
    AccentCase{0x0162, "t"}, AccentCase{0x0164, "t"},                            // Ţ Ť
    AccentCase{0x0168, "u"}, AccentCase{0x0170, "u"},                            // Ũ Ű
    AccentCase{0x0179, "z"}, AccentCase{0x017D, "z"}                             // Ź Ž
));

// The accent-table "pass-through" arms (Æ/Ð/× and out-of-table code points return
// the code point unchanged). Æ(U+00C6) lowercases to æ(U+00E6) via ToLower's
// Latin-1 arm but StripLatinAccent KEEPS it (not a base+mark) → token "æ".
TEST(WordPieceUnicodeR8, AccentTablePassThrough) {
    WordPieceTokenizer t;
    // Æ → ToLower → æ (U+00E6) → StripLatinAccent returns cp unchanged.
    EXPECT_EQ(t.BasicTokenize(U8(0x00C6)), (std::vector<std::string>{U8(0x00E6)}));
    // ð (U+00F0) is a pass-through (return cp) in the lower table.
    EXPECT_EQ(t.BasicTokenize(U8(0x00F0)), (std::vector<std::string>{U8(0x00F0)}));
    // A Latin Extended-A code point NOT in the fold table (ŉ U+0149) passes through.
    EXPECT_EQ(t.BasicTokenize(U8(0x0149)), (std::vector<std::string>{U8(0x0149)}));
}

// ToLower's × exclusion (U+00D7 MULTIPLICATION SIGN): it is in the 0x00C0–0x00DE
// window but explicitly excluded from the +0x20 lowercase shift. It is not a
// letter; BasicTokenize keeps it as a single non-letter token unchanged.
TEST(WordPieceUnicodeR8, ToLowerExcludesMultiplicationSign) {
    WordPieceTokenizer t;
    auto words = t.BasicTokenize(U8(0x00D7));
    ASSERT_EQ(words.size(), 1u);
    EXPECT_EQ(words[0], U8(0x00D7)) << "× must not be lowercased nor accent-folded";
}

// Plain ASCII uppercase → ToLower ASCII arm (A-Z → a-z).
TEST(WordPieceUnicodeR8, ToLowerAsciiUppercase) {
    WordPieceTokenizer t;
    EXPECT_EQ(t.BasicTokenize("HELLO"), (std::vector<std::string>{"hello"}));
}

// =====================================================================
// IsChineseChar: each CJK ideograph block → the char is space-padded into its own
// 1-char token (PadChineseChars). Two CJK chars with no space → two tokens.
// =====================================================================

struct CjkCase { char32_t cp; };
class CjkCharTest : public ::testing::TestWithParam<CjkCase> {};

TEST_P(CjkCharTest, EmitsOwnToken) {
    WordPieceTokenizer t;
    // Two identical CJK chars adjacent: PadChineseChars surrounds each with spaces
    // → two separate 1-char tokens (proves the IsChineseChar block arm fired).
    auto words = t.BasicTokenize(U8Seq({GetParam().cp, GetParam().cp}));
    ASSERT_EQ(words.size(), 2u) << "cp=" << std::hex << static_cast<uint32_t>(GetParam().cp);
    EXPECT_EQ(words[0], U8(GetParam().cp));
    EXPECT_EQ(words[1], U8(GetParam().cp));
}

INSTANTIATE_TEST_SUITE_P(CjkBlocks, CjkCharTest, ::testing::Values(
    CjkCase{0x4E2D},   // 中  — CJK Unified Ideographs 0x4E00–0x9FFF
    CjkCase{0x9FA5},   // 龥  — near top of the main block
    CjkCase{0x3400},   // 㐀  — CJK Ext A 0x3400–0x4DBF
    CjkCase{0x4DBF},   // Ext A top
    CjkCase{0xF900},   // 豈  — CJK Compatibility Ideographs 0xF900–0xFAFF
    CjkCase{0x20000},  // 𠀀 — CJK Ext B 0x20000–0x2A6DF (4-byte UTF-8)
    CjkCase{0x2A700},  // CJK Ext C 0x2A700–0x2B73F
    CjkCase{0x2B740},  // CJK Ext D
    CjkCase{0x2F800}   // CJK Compatibility Supplement 0x2F800–0x2FA1F
));

// A non-CJK CJK-adjacent code point (Hangul 0xAC00 / Hiragana 0x3042) is NOT in the
// IsChineseChar set → NOT space-padded (the false arm). Two adjacent Hiragana with
// no separating punctuation/space coalesce into ONE token.
TEST(WordPieceUnicodeR8, NonChineseNotPadded) {
    WordPieceTokenizer t;
    auto hira = t.BasicTokenize(U8Seq({0x3042, 0x3044}));  // あい — Hiragana, not CJK ideograph
    EXPECT_EQ(hira.size(), 1u) << "Hiragana must not be CJK-padded (single coalesced token)";
}

// =====================================================================
// IsPunctuation: ASCII sub-ranges + Unicode punctuation blocks each split off as
// their own token between two letters.
// =====================================================================

struct PunctCase { char32_t cp; };
class PunctTest : public ::testing::TestWithParam<PunctCase> {};

TEST_P(PunctTest, SplitsAsOwnToken) {
    WordPieceTokenizer t;
    // "a<punct>b" → ["a", "<punct>", "b"] (the punct char is its own token).
    auto words = t.BasicTokenize("a" + U8(GetParam().cp) + "b");
    ASSERT_EQ(words.size(), 3u) << "cp=" << std::hex << static_cast<uint32_t>(GetParam().cp);
    EXPECT_EQ(words[0], "a");
    EXPECT_EQ(words[1], U8(GetParam().cp));
    EXPECT_EQ(words[2], "b");
}

INSTANTIATE_TEST_SUITE_P(AsciiPunct, PunctTest, ::testing::Values(
    PunctCase{'!'},   // 33-47
    PunctCase{'/'},
    PunctCase{':'},   // 58-64
    PunctCase{'@'},
    PunctCase{'['},   // 91-96
    PunctCase{'`'},
    PunctCase{'{'},   // 123-126
    PunctCase{'~'}
));

INSTANTIATE_TEST_SUITE_P(UnicodePunct, PunctTest, ::testing::Values(
    PunctCase{0x2010},  // ‐ hyphen — General Punctuation 0x2010–0x2027
    PunctCase{0x2014},  // — em dash
    PunctCase{0x2027},  // ‧ hyphenation point (top of that sub-range)
    PunctCase{0x2030},  // ‰ per mille — 0x2030–0x205E
    PunctCase{0x205E},  // ⁞ vertical four dots
    PunctCase{0x3001},  // 、 CJK comma — 0x3001–0x303F
    PunctCase{0x303F},  // CJK half/full bracket (top)
    PunctCase{0xFF01},  // ！ fullwidth excl — 0xFF01–0xFF0F
    PunctCase{0xFF0F},  // ／ fullwidth solidus
    PunctCase{0xFF1A},  // ： fullwidth colon — 0xFF1A–0xFF20
    PunctCase{0xFF20}   // ＠ fullwidth at
));

// =====================================================================
// IsControl: control chars + format chars are stripped by CleanText (no token).
// IsWhitespace: Unicode space separators fold to ' ' → split words.
// =====================================================================

struct ControlCase { char32_t cp; };
class ControlStripTest : public ::testing::TestWithParam<ControlCase> {};

TEST_P(ControlStripTest, StrippedLeavingNeighbours) {
    WordPieceTokenizer t;
    // "a<ctrl>b": the control char is removed by CleanText; a/b stay adjacent → "ab".
    auto words = t.BasicTokenize("a" + U8(GetParam().cp) + "b");
    ASSERT_EQ(words.size(), 1u) << "cp=" << std::hex << static_cast<uint32_t>(GetParam().cp);
    EXPECT_EQ(words[0], "ab");
}

INSTANTIATE_TEST_SUITE_P(ControlChars, ControlStripTest, ::testing::Values(
    ControlCase{0x0001},  // C0 control (<0x20)
    ControlCase{0x001F},  // C0 top
    ControlCase{0x007F},  // DEL
    ControlCase{0x0080},  // C1 start (0x80–0x9F)
    ControlCase{0x009F},  // C1 top
    ControlCase{0x200B},  // zero-width space (Cf)
    ControlCase{0x200D},  // zero-width joiner (Cf)
    ControlCase{0x200E},  // LRM (bidi)
    ControlCase{0x202A},  // LRE (bidi format)
    ControlCase{0x202E},  // RLO (bidi format, top)
    ControlCase{0xFEFF}   // BOM / zero-width no-break space
));

struct WsCase { char32_t cp; };
class WhitespaceTest : public ::testing::TestWithParam<WsCase> {};

TEST_P(WhitespaceTest, FoldsToSpaceSplittingWords) {
    WordPieceTokenizer t;
    // "a<ws>b": the Unicode whitespace folds to ' ' → two tokens ["a","b"].
    auto words = t.BasicTokenize("a" + U8(GetParam().cp) + "b");
    ASSERT_EQ(words.size(), 2u) << "cp=" << std::hex << static_cast<uint32_t>(GetParam().cp);
    EXPECT_EQ(words[0], "a");
    EXPECT_EQ(words[1], "b");
}

INSTANTIATE_TEST_SUITE_P(UnicodeWhitespace, WhitespaceTest, ::testing::Values(
    WsCase{0x00A0},  // no-break space
    WsCase{0x1680},  // ogham space mark
    WsCase{0x2000},  // en quad
    WsCase{0x2009},  // thin space
    WsCase{0x200A},  // hair space
    WsCase{0x2028},  // line separator
    WsCase{0x2029},  // paragraph separator
    WsCase{0x202F},  // narrow no-break space
    WsCase{0x205F},  // medium mathematical space
    WsCase{0x3000}   // ideographic space
));

// ASCII whitespace variants (\t \n \r) fold to space too (the IsWhitespace ASCII arm).
TEST(WordPieceUnicodeR8, AsciiWhitespaceFolds) {
    WordPieceTokenizer t;
    EXPECT_EQ(t.BasicTokenize("a\tb\nc\rd"),
              (std::vector<std::string>{"a", "b", "c", "d"}));
}

// =====================================================================
// IsCombiningMark (StripAccents drops Mn): a base letter followed by a combining
// diacritical mark → the mark is dropped, leaving just the base letter.
// =====================================================================

struct CombiningCase { char32_t mark; };
class CombiningMarkTest : public ::testing::TestWithParam<CombiningCase> {};

TEST_P(CombiningMarkTest, DroppedLeavingBase) {
    WordPieceTokenizer t;
    // "e" + combining mark → StripAccents drops the mark → token "e".
    auto words = t.BasicTokenize("e" + U8(GetParam().mark));
    ASSERT_EQ(words.size(), 1u) << "mark=" << std::hex << static_cast<uint32_t>(GetParam().mark);
    EXPECT_EQ(words[0], "e");
}

INSTANTIATE_TEST_SUITE_P(CombiningMarks, CombiningMarkTest, ::testing::Values(
    CombiningCase{0x0300},  // combining grave — 0x0300–0x036F
    CombiningCase{0x0301},  // combining acute
    CombiningCase{0x036F},  // top of Combining Diacritical Marks
    CombiningCase{0x1AB0},  // Combining Diacritical Marks Extended
    CombiningCase{0x1DC0},  // Combining Diacritical Marks Supplement
    CombiningCase{0x20D0},  // Combining Diacritical Marks for Symbols
    CombiningCase{0xFE20}   // Combining Half Marks
));

// =====================================================================
// End-to-end Encode over a mixed multilingual string: exercises the whole
// normalizer + WordPiece + template path with the Unicode helpers all engaged at
// once (CJK pad + accent fold + punctuation split + control strip), proving the
// arms cooperate (not just in isolation).
// =====================================================================

TEST(WordPieceUnicodeR8, BasicTokenizeMixedMultilingual) {
    WordPieceTokenizer t;
    // "Café 中文!" + a zero-width joiner: Café→cafe (accent fold), 中/文 each own
    // token (CJK pad), '!' own token (punct), ZWJ stripped (control).
    auto words = t.BasicTokenize("Caf" + U8(0x00E9) + " " + U8(0x4E2D) + U8(0x6587) +
                                 "!" + U8(0x200D));
    // Expected: ["cafe", "中", "文", "!"]
    ASSERT_EQ(words.size(), 4u);
    EXPECT_EQ(words[0], "cafe");
    EXPECT_EQ(words[1], U8(0x4E2D));
    EXPECT_EQ(words[2], U8(0x6587));
    EXPECT_EQ(words[3], "!");
}

}  // namespace
}  // namespace cortrix::query
