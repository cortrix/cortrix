// R9 robustness — property-based tests for src/query/wordpiece_tokenizer.cpp.
//
// The R8 file (test_wordpiece_unicode_r8.cpp) drives every code-point RANGE arm of
// the normalizer/classifier helpers with hand-picked representatives. This file is
// the complementary *property* lane: instead of asserting one output per chosen
// input, it asserts INVARIANTS that must hold for ALL inputs rapidcheck can throw
// at the public surface — the goal is to surface boundary bugs / invariant
// violations the enumerated rows can't reach (random UTF-8, arbitrary code-point
// mixes, idempotence/determinism over the whole normalizer chain).
//
// The private helpers (CleanText/ToLower/StripAccents/PadChineseChars + the
// Is* classifiers) are static and not exposed, so — exactly like the R8 file — the
// properties drive them through the PUBLIC BasicTokenize() and Encode(). Each
// property states a structural fact that holds regardless of which range arms fire.
//
// Standalone NEW file; touches no existing test. rapidcheck via RC_GTEST_PROP.
#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

#include "cortrix/query/wordpiece_tokenizer.h"

namespace cortrix::query {
namespace {

// Encode one code point as UTF-8 (mirrors the tokenizer's own AppendUtf8; local so
// the test is self-contained and matches the R8 file's U8 helper).
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

// A rapidcheck generator for valid Unicode scalar values (skips the UTF-16
// surrogate range U+–U+DFFF, which is not a legal scalar and which our U8
// encoder would mangle). Covers BMP + the astral planes the CJK-Ext blocks live in.
rc::Gen<char32_t> genCodePoint() {
    return rc::gen::suchThat(
        rc::gen::inRange<char32_t>(1, 0x10000 + 0x2FA20),  // up to past CJK Compat Supp
        [](char32_t cp) { return cp < 0xD800 || cp > 0xDFFF; });
}

// A string built from random valid code points (the "interesting" lane: every byte
// sequence is well-formed UTF-8, so properties exercise the classifier arms rather
// than only the 0xFFFD invalid-byte path).
rc::Gen<std::string> genUnicodeString() {
    return rc::gen::map(
        rc::gen::container<std::vector<char32_t>>(genCodePoint()),
        [](const std::vector<char32_t>& cps) {
            std::string s;
            for (char32_t cp : cps) s += U8(cp);
            return s;
        });
}

// Count UTF-8 code points in a byte string (independent re-decode; the tokenizer's
// own NextCodePoint is private). Used by the truncation-budget property.
size_t CountCodePoints(const std::string& s) {
    size_t n = 0;
    for (size_t i = 0; i < s.size();) {
        unsigned char c0 = static_cast<unsigned char>(s[i]);
        if (c0 < 0x80) i += 1;
        else if ((c0 & 0xE0) == 0xC0) i += 2;
        else if ((c0 & 0xF0) == 0xE0) i += 3;
        else if ((c0 & 0xF8) == 0xF0) i += 4;
        else i += 1;
        ++n;
    }
    return n;
}

// =====================================================================
// BasicTokenize — determinism, no-empty-tokens, idempotent re-normalization.
// =====================================================================

// Determinism: the same input always yields the same token vector. A normalizer
// that read uninitialized state or depended on allocation addresses would break
// this; it must be a pure function of the bytes.
RC_GTEST_PROP(WordPiecePropR9, BasicTokenizeIsDeterministic,
              (const std::string& raw)) {
    WordPieceTokenizer t;
    RC_ASSERT(t.BasicTokenize(raw) == t.BasicTokenize(raw));
}

// Same determinism property, but over WELL-FORMED random Unicode (genUnicodeString)
// so the classifier range arms — not just ASCII — are exercised under randomness.
RC_GTEST_PROP(WordPiecePropR9, BasicTokenizeDeterministicUnicode, ()) {
    const std::string s = *genUnicodeString();
    WordPieceTokenizer t;
    RC_ASSERT(t.BasicTokenize(s) == t.BasicTokenize(s));
}

// No pre-token is ever the empty string. The pre-tokenizer's flush() guards
// !cur.empty(), and punctuation/CJK tokens are non-empty by construction; an
// off-by-one in the byte-slice bookkeeping would surface here as an empty token.
RC_GTEST_PROP(WordPiecePropR9, NoEmptyTokens, ()) {
    const std::string s = *genUnicodeString();
    WordPieceTokenizer t;
    for (const std::string& w : t.BasicTokenize(s)) {
        RC_ASSERT(!w.empty());
    }
}

// No token contains an ASCII space (0x20). Whitespace is the pre-tokenizer's split
// delimiter and is folded out by CleanText, so a surviving space inside a token
// would mean the split bookkeeping dropped a boundary.
RC_GTEST_PROP(WordPiecePropR9, TokensContainNoSpace, ()) {
    const std::string s = *genUnicodeString();
    WordPieceTokenizer t;
    for (const std::string& w : t.BasicTokenize(s)) {
        RC_ASSERT(w.find(' ') == std::string::npos);
    }
}

// Normalizer idempotence (re-feeding the concatenated normalized tokens, space-
// joined, reproduces the same token set). The normalizer chain is a projection:
// once text is cleaned/lowercased/accent-stripped/CJK-padded, running it again must
// be a fixed point. A non-idempotent arm (e.g. an accent fold that re-introduces a
// foldable form, or a lowercase that isn't a fixed point) would violate this.
RC_GTEST_PROP(WordPiecePropR9, NormalizerIsIdempotent, ()) {
    const std::string s = *genUnicodeString();
    WordPieceTokenizer t;
    std::vector<std::string> once = t.BasicTokenize(s);

    // Re-join with single spaces (a guaranteed pre-token boundary) and re-tokenize.
    std::string joined;
    for (size_t i = 0; i < once.size(); ++i) {
        if (i) joined.push_back(' ');
        joined += once[i];
    }
    std::vector<std::string> twice = t.BasicTokenize(joined);
    RC_ASSERT(once == twice);
}

// ASCII letters/digits are never dropped or altered by the normalizer: a string of
// only [a-z0-9] tokenizes to itself as a single token (lowercase ASCII is a
// normalizer fixed point, and none of clean/CJK/accent touch it).
RC_GTEST_PROP(WordPiecePropR9, AsciiLowerAlnumPreserved,
              (const std::string& seed)) {
    // Project the random seed onto [a-z0-9], non-empty.
    std::string word;
    for (char c : seed) {
        unsigned char u = static_cast<unsigned char>(c);
        word.push_back(static_cast<char>("abcdefghijklmnopqrstuvwxyz0123456789"[u % 36]));
    }
    if (word.empty()) word = "a";
    WordPieceTokenizer t;
    auto words = t.BasicTokenize(word);
    RC_ASSERT(words.size() == 1u);
    RC_ASSERT(words[0] == word);
}

// =====================================================================
// Encode (with a vocab) — shape invariants that must hold for ANY input.
// =====================================================================

// A minimal in-memory vocab covering the four required special tokens plus enough
// single-char ASCII pieces that most ASCII content tokenizes (anything else → [UNK],
// which is fine for the shape properties — they don't assert exact ids).
// LoadVocab reads a file, so we build a tiny temp vocab.txt fixture once per run.
struct TempVocab {
    std::string path;
    TempVocab() {
        std::string pattern =
            std::string(::testing::TempDir()) + "/wp_prop_vocab_XXXXXX";
        std::vector<char> path_buffer(pattern.begin(), pattern.end());
        path_buffer.push_back('\0');
        const int fd = ::mkstemp(path_buffer.data());
        if (fd < 0) {
            throw std::runtime_error("failed to create temporary WordPiece vocabulary");
        }
        path = path_buffer.data();
        FILE* f = ::fdopen(fd, "w");
        if (f == nullptr) {
            ::close(fd);
            std::remove(path.c_str());
            throw std::runtime_error("failed to open temporary WordPiece vocabulary");
        }

        bool write_ok = true;
        // ids by line: keep [PAD] at 0 (matches the real DistilBERT layout).
        write_ok = std::fputs("[PAD]\n[UNK]\n[CLS]\n[SEP]\n", f) != EOF;
        for (char c = 'a'; c <= 'z'; ++c) {
            write_ok = write_ok && std::fputc(c, f) != EOF;
            write_ok = write_ok && std::fputc('\n', f) != EOF;
        }
        for (char c = '0'; c <= '9'; ++c) {
            write_ok = write_ok && std::fputc(c, f) != EOF;
            write_ok = write_ok && std::fputc('\n', f) != EOF;
        }
        // a couple of ## continuation pieces so multi-char words can chain.
        write_ok = write_ok && std::fputs("##a\n##b\n##c\n", f) != EOF;
        const int close_result = std::fclose(f);
        if (!write_ok || close_result != 0) {
            std::remove(path.c_str());
            throw std::runtime_error("failed to write temporary WordPiece vocabulary");
        }
    }

    ~TempVocab() { std::remove(path.c_str()); }
};

const TempVocab& Vocab() {
    static TempVocab v;  // built once
    return v;
}

// Encode never throws and always returns Ok for max_length >= 2; the output always
// begins with [CLS] and ends with [SEP], and input_ids.size() == attention_mask.size()
// with the mask all-ones. This must hold for arbitrary (possibly malformed) bytes.
RC_GTEST_PROP(WordPiecePropR9, EncodeShapeInvariants,
              (const std::string& raw)) {
    WordPieceTokenizer t;
    RC_ASSERT(t.LoadVocab(Vocab().path).ok());

    // max_length anywhere in [2, 130] — covers the truncation boundary.
    const int max_length = *rc::gen::inRange(2, 131);
    WordPieceTokenizer::Encoded enc;
    Status st = t.Encode(raw, max_length, &enc);
    RC_ASSERT(st.ok());

    RC_ASSERT(enc.input_ids.size() >= 2u);                 // at least [CLS] [SEP]
    RC_ASSERT(enc.input_ids.front() == t.cls_id());
    RC_ASSERT(enc.input_ids.back() == t.sep_id());
    RC_ASSERT(enc.input_ids.size() <= static_cast<size_t>(max_length));
    RC_ASSERT(enc.attention_mask.size() == enc.input_ids.size());
    for (int64_t m : enc.attention_mask) RC_ASSERT(m == 1);
}

// Truncation budget: the number of CONTENT tokens (everything between [CLS]/[SEP])
// never exceeds max_length-2, for any input and any max_length. This is the CRAG/query routing
// truncation_side=right contract; an off-by-one in the budget check would break it.
RC_GTEST_PROP(WordPiecePropR9, EncodeRespectsTruncationBudget,
              (const std::string& raw)) {
    WordPieceTokenizer t;
    RC_ASSERT(t.LoadVocab(Vocab().path).ok());
    const int max_length = *rc::gen::inRange(2, 131);
    WordPieceTokenizer::Encoded enc;
    RC_ASSERT(t.Encode(raw, max_length, &enc).ok());
    const size_t content = enc.input_ids.size() - 2;  // strip [CLS]/[SEP]
    RC_ASSERT(content <= static_cast<size_t>(max_length) - 2);
}

// Empty / whitespace-only input yields exactly [CLS] [SEP] (the documented
// contract: "an empty/whitespace-only input yields just [CLS] [SEP]"). Generated
// across random run-lengths of assorted whitespace.
RC_GTEST_PROP(WordPiecePropR9, WhitespaceOnlyYieldsClsSep, ()) {
    static const char32_t kWs[] = {' ', '\t', '\n', '\r', 0x00A0, 0x2003, 0x3000};
    const auto idxs = *rc::gen::container<std::vector<size_t>>(
        rc::gen::inRange<size_t>(0, sizeof(kWs) / sizeof(kWs[0])));
    std::string s;
    for (size_t i : idxs) s += U8(kWs[i]);

    WordPieceTokenizer t;
    RC_ASSERT(t.LoadVocab(Vocab().path).ok());
    WordPieceTokenizer::Encoded enc;
    RC_ASSERT(t.Encode(s, 32, &enc).ok());
    RC_ASSERT(enc.input_ids.size() == 2u);
    RC_ASSERT(enc.input_ids[0] == t.cls_id());
    RC_ASSERT(enc.input_ids[1] == t.sep_id());
}

// WordPieceEncode over a single pre-token: a word longer than
// max_input_chars_per_word(=100) code points collapses to exactly one [UNK] id (the
// kMaxInputCharsPerWord guard), for any code-point makeup of that long word.
RC_GTEST_PROP(WordPiecePropR9, OverlongWordIsSingleUnk, ()) {
    // 101..160 copies of a single ASCII letter → guaranteed > 100 code points and a
    // single pre-token (no whitespace/punct/CJK inside).
    const int count = *rc::gen::inRange(101, 161);
    const char letter = static_cast<char>('a' + *rc::gen::inRange(0, 26));
    std::string word(static_cast<size_t>(count), letter);

    WordPieceTokenizer t;
    RC_ASSERT(t.LoadVocab(Vocab().path).ok());
    auto ids = t.WordPieceEncode(word);
    RC_ASSERT(ids.size() == 1u);
    RC_ASSERT(ids[0] == t.unk_id());
}

// CJK characters are each split into their own pre-token (handle_chinese_chars). A
// run of N adjacent CJK ideographs (no other chars) yields exactly N tokens.
RC_GTEST_PROP(WordPiecePropR9, CjkRunSplitsPerChar, ()) {
    // Random length run of code points drawn from the main CJK block 0x4E00–0x9FFF.
    const int n = *rc::gen::inRange(1, 20);
    std::string s;
    std::vector<char32_t> cps;
    for (int i = 0; i < n; ++i) {
        char32_t cp = *rc::gen::inRange<char32_t>(0x4E00, 0x9FFF + 1);
        cps.push_back(cp);
        s += U8(cp);
    }
    WordPieceTokenizer t;
    auto words = t.BasicTokenize(s);
    RC_ASSERT(words.size() == static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) RC_ASSERT(words[i] == U8(cps[i]));
}

// Total code points across all tokens never exceeds the input's code-point count:
// the normalizer only drops (controls, accents, surplus whitespace) or pads with
// ASCII spaces (which become boundaries, not token chars) — it never invents
// letters. So sum(|token|_cp) <= |input|_cp for well-formed Unicode input.
RC_GTEST_PROP(WordPiecePropR9, TokenCharsDoNotExceedInput, ()) {
    const std::string s = *genUnicodeString();
    WordPieceTokenizer t;
    size_t total = 0;
    for (const std::string& w : t.BasicTokenize(s)) total += CountCodePoints(w);
    RC_ASSERT(total <= CountCodePoints(s));
}

}  // namespace
}  // namespace cortrix::query
