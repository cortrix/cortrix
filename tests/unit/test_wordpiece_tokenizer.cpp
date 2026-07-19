#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "cortrix/query/wordpiece_tokenizer.h"

// WordPieceTokenizer coverage: the ids below are GOLDEN — generated offline with
// HuggingFace `AutoTokenizer.from_pretrained(models/query-complexity)`
// (distilbert-base-uncased, do_lower_case, strip_accents, handle_chinese_chars,
// truncation max_length=64) and written down here so the C++ WordPiece pipeline
// is verified byte-for-byte against the real tokenizer the model was trained with.
// They cover lowercasing, punctuation splitting, accent stripping (precomposed +
// already-uncased), CJK per-char + [UNK], an emoji [UNK], 64-token truncation, and
// the empty / whitespace-only degenerate inputs. See the parent task brief.

namespace cortrix::query {
namespace {

namespace fs = std::filesystem;

// vocab.txt lives next to model.onnx. Tests that need it skip when the model
// archive is absent (CI / offline), exactly like the ONNX model tests.
std::string VocabPath() {
    return std::string(CORTRIX_CE_SOURCE_DIR) +
           "/models/query-complexity/vocab.txt";
}

WordPieceTokenizer LoadOrSkip() {
    WordPieceTokenizer tk;
    if (!fs::exists(VocabPath())) {
        return tk;  // not loaded; caller checks loaded() and SKIPs
    }
    EXPECT_TRUE(tk.LoadVocab(VocabPath()).ok());
    return tk;
}

// Each golden case: name, input text, full expected input_ids (incl [CLS]/[SEP]).
struct GoldenCase {
    const char* name;
    const char* text;
    std::vector<int64_t> ids;
};

std::vector<int64_t> Encode64(const WordPieceTokenizer& tk, const std::string& text) {
    WordPieceTokenizer::Encoded enc;
    EXPECT_TRUE(tk.Encode(text, 64, &enc).ok());
    // attention_mask is all 1s and the same length as input_ids (no padding).
    EXPECT_EQ(enc.attention_mask.size(), enc.input_ids.size());
    for (int64_t m : enc.attention_mask) EXPECT_EQ(m, 1);
    return enc.input_ids;
}

TEST(WordPieceTokenizerTest, GoldenParityWithHuggingFace) {
    WordPieceTokenizer tk = LoadOrSkip();
    if (!tk.loaded()) GTEST_SKIP() << "query-complexity vocab.txt absent (offline build)";

    const std::vector<GoldenCase> cases = {
        {"greeting_punct", "Hello, thanks!",
         {101, 7592, 1010, 4283, 999, 102}},
        {"what_is", "What is Cortrix?",
         {101, 2054, 2003, 2522, 5339, 17682, 1029, 102}},
        {"mixed_case", "The Quick BROWN Fox.",
         {101, 1996, 4248, 2829, 4419, 1012, 102}},
        {"accents_ascii", "naive cafe resume",
         {101, 15743, 7668, 13746, 102}},
        {"accents_unicode", "na\xc3\xafve caf\xc3\xa9 r\xc3\xa9sum\xc3\xa9",  // naïve café résumé
         {101, 15743, 7668, 13746, 102}},
        // CJK "ni hao shi jie" as UTF-8 bytes; vocab has only U+4E16 (1745),
        // the other three ideographs map to [UNK] (100).
        {"chinese", "\xe4\xbd\xa0\xe5\xa5\xbd\xe4\xb8\x96\xe7\x95\x8c",
         {101, 100, 100, 1745, 100, 102}},
        {"cpp_colon", "C++ and Rust: a comparison.",
         {101, 1039, 1009, 1009, 1998, 18399, 1024, 1037, 7831, 1012, 102}},
        {"email_bang", "Email me at foo@bar.com ASAP!!!",
         {101, 10373, 2033, 2012, 29379, 1030, 3347, 1012, 4012, 17306, 2361, 999, 999, 999, 102}},
        {"emoji_unk", "cool \xf0\x9f\x98\x80 stuff",  // cool 😀 stuff
         {101, 4658, 100, 4933, 102}},
    };

    for (const auto& c : cases) {
        SCOPED_TRACE(c.name);
        EXPECT_EQ(Encode64(tk, c.text), c.ids);
    }
}

TEST(WordPieceTokenizerTest, TruncatesToMaxLength) {
    WordPieceTokenizer tk = LoadOrSkip();
    if (!tk.loaded()) GTEST_SKIP() << "vocab absent";

    // 80 repeats of "word" (token id 2773) → HF truncates to 64 total:
    // [CLS] + 62 content + [SEP].
    std::string text;
    for (int i = 0; i < 80; ++i) text += "word ";
    auto ids = Encode64(tk, text);

    ASSERT_EQ(ids.size(), 64u);
    EXPECT_EQ(ids.front(), tk.cls_id());
    EXPECT_EQ(ids.back(), tk.sep_id());
    for (size_t i = 1; i + 1 < ids.size(); ++i) EXPECT_EQ(ids[i], 2773);
}

TEST(WordPieceTokenizerTest, EmptyAndWhitespaceAreClsSepOnly) {
    WordPieceTokenizer tk = LoadOrSkip();
    if (!tk.loaded()) GTEST_SKIP() << "vocab absent";

    const std::vector<int64_t> cls_sep = {tk.cls_id(), tk.sep_id()};
    EXPECT_EQ(Encode64(tk, ""), cls_sep);
    EXPECT_EQ(Encode64(tk, "   "), cls_sep);
    EXPECT_EQ(Encode64(tk, " \t\n "), cls_sep);
}

TEST(WordPieceTokenizerTest, SpecialTokenIdsResolved) {
    WordPieceTokenizer tk = LoadOrSkip();
    if (!tk.loaded()) GTEST_SKIP() << "vocab absent";
    EXPECT_EQ(tk.cls_id(), 101);
    EXPECT_EQ(tk.sep_id(), 102);
    EXPECT_EQ(tk.unk_id(), 100);
    EXPECT_EQ(tk.pad_id(), 0);
    EXPECT_GT(tk.vocab_size(), 30000u);  // distilbert-base-uncased = 30522
}

TEST(WordPieceTokenizerTest, WordPieceContinuationPrefix) {
    WordPieceTokenizer tk = LoadOrSkip();
    if (!tk.loaded()) GTEST_SKIP() << "vocab absent";

    // "antidisestablishmentarianism" is a single long word that WordPiece splits
    // into several "##" continuation subwords (golden ids from HF tokenize()).
    auto ids = tk.WordPieceEncode("antidisestablishmentarianism");
    const std::vector<int64_t> expected = {3424, 10521, 4355, 7875, 13602, 3672, 12199, 2964};
    EXPECT_EQ(ids, expected);
}

TEST(WordPieceTokenizerTest, OverlongWordIsUnk) {
    WordPieceTokenizer tk = LoadOrSkip();
    if (!tk.loaded()) GTEST_SKIP() << "vocab absent";

    // A single "word" of >100 chars exceeds max_input_chars_per_word → [UNK].
    std::string huge(150, 'a');
    auto ids = tk.WordPieceEncode(huge);
    ASSERT_EQ(ids.size(), 1u);
    EXPECT_EQ(ids[0], tk.unk_id());
}

TEST(WordPieceTokenizerTest, BasicTokenizeSplitsPunctuationAndLowercases) {
    WordPieceTokenizer tk = LoadOrSkip();
    if (!tk.loaded()) GTEST_SKIP() << "vocab absent";

    // Pure normalizer + pre-tokenizer view (no special tokens): punctuation is
    // its own pre-token, letters are lowercased.
    auto words = tk.BasicTokenize("Hi, World!");
    const std::vector<std::string> expected = {"hi", ",", "world", "!"};
    EXPECT_EQ(words, expected);
}

TEST(WordPieceTokenizerTest, EncodeFailsWithoutVocab) {
    WordPieceTokenizer tk;  // never loaded
    EXPECT_FALSE(tk.loaded());
    WordPieceTokenizer::Encoded enc;
    EXPECT_FALSE(tk.Encode("hello", 64, &enc).ok());
}

TEST(WordPieceTokenizerTest, LoadVocabMissingFileIsNotFound) {
    WordPieceTokenizer tk;
    auto st = tk.LoadVocab("/nonexistent/path/vocab.txt");
    EXPECT_FALSE(st.ok());
    EXPECT_EQ(st.code(), StatusCode::kNotFound);
}

}  // namespace
}  // namespace cortrix::query
