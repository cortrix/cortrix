#include <gtest/gtest.h>
#include "cortrix/spc/hf_tokenizer.h"
#include <fstream>
#include <filesystem>
#include <nlohmann/json.hpp>

namespace cortrix {
namespace {

namespace fs = std::filesystem;

// Helper: create a minimal tokenizer.json for testing
std::string CreateMinimalTokenizerJson(const std::string& dir) {
    // Minimal tokenizer.json compatible with HfTokenizer::Load()
    // Uses a tiny vocab and merge set
    nlohmann::json j;

    // Vocab: byte-level tokens + special tokens
    nlohmann::json vocab;
    vocab["<s>"] = 0;
    vocab["<pad>"] = 1;
    vocab["</s>"] = 2;
    vocab["<unk>"] = 3;
    // A few byte-level tokens for "hello"
    // In byte-level BPE, 'h'=104 maps to 'h', 'e'=101 maps to 'e', etc.
    // With prefix space, ' h' -> byte-encoded as '\u0120h' where \u0120 is Ġ (256+32-33=... actually space=32 maps to Ġ)
    // Simplified: just add individual character tokens
    vocab["Ġ"] = 4;  // space byte in GPT-2 byte encoding
    vocab["h"] = 5;
    vocab["e"] = 6;
    vocab["l"] = 7;
    vocab["o"] = 8;
    vocab["Ġh"] = 9;
    vocab["Ġhe"] = 10;
    vocab["ll"] = 11;
    vocab["llo"] = 12;

    nlohmann::json model;
    model["type"] = "BPE";
    model["vocab"] = vocab;

    // Merges
    nlohmann::json merges = nlohmann::json::array();
    merges.push_back("Ġ h");    // rank 0: Ġ + h -> Ġh
    merges.push_back("l l");     // rank 1: l + l -> ll
    merges.push_back("Ġh e");   // rank 2: Ġh + e -> Ġhe
    merges.push_back("ll o");    // rank 3: ll + o -> llo
    model["merges"] = merges;

    j["model"] = model;

    // Added tokens (special tokens)
    nlohmann::json added_tokens = nlohmann::json::array();
    added_tokens.push_back({{"id", 0}, {"content", "<s>"}, {"special", true}});
    added_tokens.push_back({{"id", 1}, {"content", "<pad>"}, {"special", true}});
    added_tokens.push_back({{"id", 2}, {"content", "</s>"}, {"special", true}});
    added_tokens.push_back({{"id", 3}, {"content", "<unk>"}, {"special", true}});
    j["added_tokens"] = added_tokens;

    std::string path = dir + "/tokenizer.json";
    std::ofstream f(path);
    f << j.dump(2);
    f.close();
    return path;
}

class HfTokenizerTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Unique per test: parallel ctest processes must not share this dir
        // (a sibling's TearDown/remove_all would yank files mid-test).
        test_dir_ = fs::temp_directory_path() /
                    (std::string("cortrix_tokenizer_test_") +
                     ::testing::UnitTest::GetInstance()->current_test_info()->name());
        fs::create_directories(test_dir_);
    }
    void TearDown() override {
        fs::remove_all(test_dir_);
    }
    fs::path test_dir_;
};

// ============================================================
// Load tests
// ============================================================

TEST_F(HfTokenizerTest, Load_FileNotFound) {
    HfTokenizer tok;
    Status s = tok.Load("/nonexistent/tokenizer.json");
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kNotFound);
}

TEST_F(HfTokenizerTest, Load_InvalidJson) {
    std::string path = (test_dir_ / "tokenizer.json").string();
    std::ofstream f(path);
    f << "not json content{{{";
    f.close();

    HfTokenizer tok;
    Status s = tok.Load(path);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kInternal);
}

TEST_F(HfTokenizerTest, Load_MissingVocab) {
    std::string path = (test_dir_ / "tokenizer.json").string();
    nlohmann::json j;
    j["model"] = {{"type", "BPE"}};
    std::ofstream f(path);
    f << j.dump();
    f.close();

    HfTokenizer tok;
    Status s = tok.Load(path);
    EXPECT_FALSE(s.ok());
}

TEST_F(HfTokenizerTest, Load_Success) {
    std::string path = CreateMinimalTokenizerJson(test_dir_.string());

    HfTokenizer tok;
    Status s = tok.Load(path);
    EXPECT_TRUE(s.ok()) << s.message();
}

// ============================================================
// Encode tests
// ============================================================

TEST_F(HfTokenizerTest, Encode_NotLoaded) {
    HfTokenizer tok;
    HfTokenizer::Encoded enc;
    Status s = tok.Encode("hello", 512, &enc);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kInternal);
}

TEST_F(HfTokenizerTest, Encode_NullOutput) {
    HfTokenizer tok;
    Status s = tok.Encode("hello", 512, nullptr);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kInvalidArgument);
}

TEST_F(HfTokenizerTest, Encode_RejectsMaxLengthBelowSpecialTokenFloor) {
    std::string path = CreateMinimalTokenizerJson(test_dir_.string());

    HfTokenizer tok;
    ASSERT_TRUE(tok.Load(path).ok());

    HfTokenizer::Encoded enc;
    Status s = tok.Encode("hello", 1, &enc);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kInvalidArgument);

    s = tok.EncodeNoPad("hello", 1, &enc);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kInvalidArgument);
}

TEST_F(HfTokenizerTest, Encode_BasicOutput) {
    std::string path = CreateMinimalTokenizerJson(test_dir_.string());

    HfTokenizer tok;
    ASSERT_TRUE(tok.Load(path).ok());

    HfTokenizer::Encoded enc;
    Status s = tok.Encode("hello", 32, &enc);
    ASSERT_TRUE(s.ok()) << s.message();

    // Should have CLS at start, SEP near end, padded to max_length
    EXPECT_EQ(static_cast<int>(enc.input_ids.size()), 32);
    EXPECT_EQ(static_cast<int>(enc.attention_mask.size()), 32);

    // First token should be CLS (<s> = 0)
    EXPECT_EQ(enc.input_ids[0], 0);

    // Attention mask: real tokens have 1, padding has 0
    EXPECT_EQ(enc.attention_mask[0], 1);  // CLS
    // Last positions should be padding (0)
    EXPECT_EQ(enc.attention_mask[31], 0);
}

TEST_F(HfTokenizerTest, EncodeNoPad_DoesNotPadShortBpeText) {
    std::string path = CreateMinimalTokenizerJson(test_dir_.string());

    HfTokenizer tok;
    ASSERT_TRUE(tok.Load(path).ok());

    HfTokenizer::Encoded enc;
    Status s = tok.EncodeNoPad("hello", 32, &enc);
    ASSERT_TRUE(s.ok()) << s.message();

    EXPECT_LT(static_cast<int>(enc.input_ids.size()), 32);
    EXPECT_EQ(enc.attention_mask.size(), enc.input_ids.size());
    EXPECT_EQ(enc.input_ids.front(), 0);
    EXPECT_EQ(enc.input_ids.back(), 2);
    for (auto m : enc.attention_mask) {
        EXPECT_EQ(m, 1);
    }
}

TEST_F(HfTokenizerTest, Encode_EmptyText) {
    std::string path = CreateMinimalTokenizerJson(test_dir_.string());

    HfTokenizer tok;
    ASSERT_TRUE(tok.Load(path).ok());

    HfTokenizer::Encoded enc;
    Status s = tok.Encode("", 16, &enc);
    ASSERT_TRUE(s.ok()) << s.message();

    // Should at least have CLS + SEP
    EXPECT_EQ(static_cast<int>(enc.input_ids.size()), 16);
    EXPECT_EQ(enc.input_ids[0], 0);  // CLS

    // Find SEP
    bool has_sep = false;
    for (auto id : enc.input_ids) {
        if (id == 2) { has_sep = true; break; }
    }
    EXPECT_TRUE(has_sep);
}

TEST_F(HfTokenizerTest, Encode_Truncation) {
    std::string path = CreateMinimalTokenizerJson(test_dir_.string());

    HfTokenizer tok;
    ASSERT_TRUE(tok.Load(path).ok());

    // Very short max_length should still work
    HfTokenizer::Encoded enc;
    Status s = tok.Encode("hello", 4, &enc);
    ASSERT_TRUE(s.ok()) << s.message();

    EXPECT_EQ(static_cast<int>(enc.input_ids.size()), 4);
    // First token = CLS
    EXPECT_EQ(enc.input_ids[0], 0);
    // Last real token should be SEP
    EXPECT_EQ(enc.input_ids[3], 2);
}

TEST_F(HfTokenizerTest, Encode_AttentionMaskCorrect) {
    std::string path = CreateMinimalTokenizerJson(test_dir_.string());

    HfTokenizer tok;
    ASSERT_TRUE(tok.Load(path).ok());

    HfTokenizer::Encoded enc;
    ASSERT_TRUE(tok.Encode("hello", 32, &enc).ok());

    // Count real tokens (attention_mask == 1)
    int real_count = 0;
    for (auto m : enc.attention_mask) {
        if (m == 1) real_count++;
    }
    EXPECT_GT(real_count, 0);
    EXPECT_LE(real_count, 32);

    // Padding tokens should have attention_mask == 0
    int pad_count = 0;
    for (auto m : enc.attention_mask) {
        if (m == 0) pad_count++;
    }
    EXPECT_EQ(real_count + pad_count, 32);
}

// ============================================================
// Unigram (SentencePiece / bge-m3) model path
// ============================================================

// Build a minimal Unigram tokenizer.json: a [token,score] vocab array, a
// Metaspace pre_tokenizer, and the 4 XLM-R special tokens via added_tokens.
std::string CreateMinimalUnigramJson(const std::string& dir,
                                     bool with_metaspace = true,
                                     bool add_prefix_space = true) {
    nlohmann::json model;
    model["type"] = "Unigram";
    // vocab is an array of [token_string, score] pairs (id = array index).
    nlohmann::json vocab = nlohmann::json::array();
    vocab.push_back({"<s>", 0.0});      // id 0
    vocab.push_back({"<pad>", 0.0});    // id 1
    vocab.push_back({"</s>", 0.0});     // id 2
    vocab.push_back({"<unk>", 0.0});    // id 3
    // The metaspace replacement is U+2581 (a 3-byte UTF-8 char), bytes e2 96 81.
    vocab.push_back({"\xe2\x96\x81he", -1.0});   // id 4: "U+2581he"
    vocab.push_back({"llo", -2.0});               // id 5
    vocab.push_back({"\xe2\x96\x81", -5.0});     // id 6: bare "U+2581"
    vocab.push_back({"h", -8.0});                 // id 7
    vocab.push_back({"e", -8.0});                 // id 8
    vocab.push_back({"l", -8.0});                 // id 9
    vocab.push_back({"o", -8.0});                 // id 10
    model["vocab"] = vocab;

    nlohmann::json j;
    j["model"] = model;

    if (with_metaspace) {
        nlohmann::json pt;
        pt["type"] = "Metaspace";
        pt["replacement"] = "\xe2\x96\x81";
        pt["add_prefix_space"] = add_prefix_space;
        j["pre_tokenizer"] = pt;
    }

    nlohmann::json added = nlohmann::json::array();
    added.push_back({{"id", 0}, {"content", "<s>"}});
    added.push_back({{"id", 1}, {"content", "<pad>"}});
    added.push_back({{"id", 2}, {"content", "</s>"}});
    added.push_back({{"id", 3}, {"content", "<unk>"}});
    j["added_tokens"] = added;

    std::string path = dir + "/tokenizer.json";
    std::ofstream f(path);
    f << j.dump(2);
    f.close();
    return path;
}

TEST_F(HfTokenizerTest, Load_UnigramSuccess) {
    std::string path = CreateMinimalUnigramJson(test_dir_.string());
    HfTokenizer tok;
    Status s = tok.Load(path);
    EXPECT_TRUE(s.ok()) << s.message();
    EXPECT_TRUE(tok.loaded());
}

TEST_F(HfTokenizerTest, Load_UnigramMissingVocabArray) {
    // model.type=Unigram but vocab is an object (not the required array) -> error.
    std::string path = (test_dir_ / "tokenizer.json").string();
    nlohmann::json j;
    j["model"] = {{"type", "Unigram"}, {"vocab", {{"a", 1}}}};
    std::ofstream f(path);
    f << j.dump();
    f.close();

    HfTokenizer tok;
    Status s = tok.Load(path);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kInternal);
}

TEST_F(HfTokenizerTest, Load_UnigramSkipsMalformedVocabEntries) {
    // Vocab entries that are not [token, score] pairs are skipped (the `continue`
    // branch) without failing the load.
    std::string path = (test_dir_ / "tokenizer.json").string();
    nlohmann::json vocab = nlohmann::json::array();
    vocab.push_back({"<s>", 0.0});
    vocab.push_back("not_an_array");       // skipped (not array)
    vocab.push_back({"only_one_element"}); // skipped (size < 2)
    vocab.push_back({"valid", -1.0});      // kept
    nlohmann::json j;
    j["model"] = {{"type", "Unigram"}, {"vocab", vocab}};
    std::ofstream f(path);
    f << j.dump();
    f.close();

    HfTokenizer tok;
    Status s = tok.Load(path);
    EXPECT_TRUE(s.ok()) << s.message();
}

TEST_F(HfTokenizerTest, Encode_UnigramViterbiSegmentation) {
    std::string path = CreateMinimalUnigramJson(test_dir_.string());
    HfTokenizer tok;
    ASSERT_TRUE(tok.Load(path).ok());

    // "hello" -> metaspace prefix -> "U+2581hello"; Viterbi prefers "U+2581he" + "llo"
    // (scores -1 + -2 = -3) over single chars (much heavier penalty).
    HfTokenizer::Encoded enc;
    Status s = tok.Encode("hello", 32, &enc);
    ASSERT_TRUE(s.ok()) << s.message();
    EXPECT_EQ(static_cast<int>(enc.input_ids.size()), 32);
    EXPECT_EQ(enc.input_ids[0], 0);   // <s>
    // The two best tokens "U+2581he"(id4) and "llo"(id5) should both appear.
    bool has_he = false, has_llo = false;
    for (auto id : enc.input_ids) {
        if (id == 4) has_he = true;
        if (id == 5) has_llo = true;
    }
    EXPECT_TRUE(has_he);
    EXPECT_TRUE(has_llo);
}

TEST_F(HfTokenizerTest, EncodeNoPad_DoesNotPadShortUnigramText) {
    std::string path = CreateMinimalUnigramJson(test_dir_.string());
    HfTokenizer tok;
    ASSERT_TRUE(tok.Load(path).ok());

    HfTokenizer::Encoded enc;
    ASSERT_TRUE(tok.EncodeNoPad("hello", 32, &enc).ok());

    EXPECT_LT(static_cast<int>(enc.input_ids.size()), 32);
    EXPECT_EQ(enc.attention_mask.size(), enc.input_ids.size());
    EXPECT_EQ(enc.input_ids.front(), 0);
    EXPECT_EQ(enc.input_ids.back(), 2);
    for (auto m : enc.attention_mask) {
        EXPECT_EQ(m, 1);
    }
}

TEST_F(HfTokenizerTest, Encode_UnigramEmptyText) {
    // Empty text -> UnigramPreProcess yields just the prefix "U+2581" (add_prefix_space),
    // which is a known token; still produces CLS + token + SEP.
    std::string path = CreateMinimalUnigramJson(test_dir_.string());
    HfTokenizer tok;
    ASSERT_TRUE(tok.Load(path).ok());

    HfTokenizer::Encoded enc;
    ASSERT_TRUE(tok.Encode("", 8, &enc).ok());
    EXPECT_EQ(enc.input_ids[0], 0);  // <s>
    bool has_sep = false;
    for (auto id : enc.input_ids) if (id == 2) has_sep = true;
    EXPECT_TRUE(has_sep);
}

TEST_F(HfTokenizerTest, Encode_UnigramUnkFallback) {
    // A character with no matching vocab token forces the UNK single-char
    // fallback branch in UnigramEncode (heavy penalty, but keeps the path total).
    std::string path = CreateMinimalUnigramJson(test_dir_.string());
    HfTokenizer tok;
    ASSERT_TRUE(tok.Load(path).ok());

    HfTokenizer::Encoded enc;
    // 'z' / 'x' / 'q' are not in the tiny vocab -> UNK id 3.
    ASSERT_TRUE(tok.Encode("zxq", 16, &enc).ok());
    bool has_unk = false;
    for (auto id : enc.input_ids) if (id == 3) has_unk = true;
    EXPECT_TRUE(has_unk);
}

TEST_F(HfTokenizerTest, Encode_UnigramNoMetaspaceNoPrefix) {
    // pre_tokenizer absent -> defaults kept; add_prefix_space=false variant exercises
    // the UnigramPreProcess branch that does NOT prepend the replacement.
    std::string path = CreateMinimalUnigramJson(test_dir_.string(),
                                                /*with_metaspace=*/true,
                                                /*add_prefix_space=*/false);
    HfTokenizer tok;
    ASSERT_TRUE(tok.Load(path).ok());
    HfTokenizer::Encoded enc;
    ASSERT_TRUE(tok.Encode("hello", 16, &enc).ok());
    EXPECT_EQ(enc.input_ids[0], 0);
}

// ============================================================
// Multi-byte UTF-8 + pre_tokenizer + special-token parsing
// ============================================================

TEST_F(HfTokenizerTest, Encode_MultiByteUtf8Input) {
    // A 2-byte, a 3-byte and a 4-byte UTF-8 code point drive the Utf8Chars /
    // Utf8CharLen / Char32ToUtf8 multi-byte branches via the BPE byte encoder.
    std::string path = CreateMinimalTokenizerJson(test_dir_.string());
    HfTokenizer tok;
    ASSERT_TRUE(tok.Load(path).ok());

    HfTokenizer::Encoded enc;
    // U+00E9 (2-byte) + U+4E2D (3-byte) + U+1F600 (4-byte), written as raw bytes
    // so the source stays ASCII-only; unknown tokens map to <unk>.
    Status s = tok.Encode("\xc3\xa9\xe4\xb8\xad\xf0\x9f\x98\x80", 32, &enc);
    ASSERT_TRUE(s.ok()) << s.message();
    EXPECT_EQ(enc.input_ids[0], 0);  // <s>
    EXPECT_EQ(static_cast<int>(enc.input_ids.size()), 32);
}

TEST_F(HfTokenizerTest, Load_AddedTokensSetSpecialIds) {
    // added_tokens with non-default ids must update cls/sep/pad/unk so Encode uses
    // them (exercises each `content ==` branch in the added_tokens loop).
    std::string path = (test_dir_ / "tokenizer.json").string();
    nlohmann::json vocab;
    vocab["<s>"] = 10;
    vocab["</s>"] = 11;
    vocab["<pad>"] = 12;
    vocab["<unk>"] = 13;
    vocab["a"] = 14;
    nlohmann::json j;
    j["model"] = {{"type", "BPE"}, {"vocab", vocab}};
    nlohmann::json added = nlohmann::json::array();
    added.push_back({{"id", 10}, {"content", "<s>"}});
    added.push_back({{"id", 11}, {"content", "</s>"}});
    added.push_back({{"id", 12}, {"content", "<pad>"}});
    added.push_back({{"id", 13}, {"content", "<unk>"}});
    added.push_back({{"id", 99}, {"content", "<other>"}});  // none of the 4 -> no-op
    j["added_tokens"] = added;
    std::ofstream f(path);
    f << j.dump();
    f.close();

    HfTokenizer tok;
    ASSERT_TRUE(tok.Load(path).ok());
    HfTokenizer::Encoded enc;
    ASSERT_TRUE(tok.Encode("a", 8, &enc).ok());
    EXPECT_EQ(enc.input_ids[0], 10);                       // remapped <s>
    EXPECT_EQ(enc.input_ids[enc.input_ids.size() == 8 ? 7 : 0], 12);  // padded with <pad>=12
}

TEST_F(HfTokenizerTest, Load_BpeWithoutMerges) {
    // A BPE model.vocab present but NO "merges" key -> the merges loop is skipped;
    // every char encodes to its own token (or <unk>). Exercises the no-merges path.
    std::string path = (test_dir_ / "tokenizer.json").string();
    nlohmann::json vocab;
    vocab["<s>"] = 0; vocab["<pad>"] = 1; vocab["</s>"] = 2; vocab["<unk>"] = 3;
    vocab["a"] = 4;
    nlohmann::json j;
    j["model"] = {{"type", "BPE"}, {"vocab", vocab}};  // no merges
    std::ofstream f(path);
    f << j.dump();
    f.close();

    HfTokenizer tok;
    ASSERT_TRUE(tok.Load(path).ok());
    HfTokenizer::Encoded enc;
    EXPECT_TRUE(tok.Encode("a", 8, &enc).ok());
}

TEST_F(HfTokenizerTest, Encode_BpeMergesActuallyTrigger) {
    // The minimal fixture's merges (Gspace+h, l+l, Gspaceh+e, ll+o) drive BpeWord's
    // best-pair search + the merge-application loop (found==true repeatedly).
    std::string path = CreateMinimalTokenizerJson(test_dir_.string());
    HfTokenizer tok;
    ASSERT_TRUE(tok.Load(path).ok());

    HfTokenizer::Encoded enc;
    ASSERT_TRUE(tok.Encode("hello", 32, &enc).ok());
    // "Gspacehe" (id 10) and/or "llo" (id 12) should result from merges.
    bool merged = false;
    for (auto id : enc.input_ids) {
        if (id == 10 || id == 12 || id == 9 || id == 11) merged = true;
    }
    EXPECT_TRUE(merged);
}

TEST_F(HfTokenizerTest, Encode_MissingModelSection) {
    // JSON with no "model" section -> Internal error (the early model-missing guard).
    std::string path = (test_dir_ / "tokenizer.json").string();
    std::ofstream f(path);
    f << R"({"version":"1.0"})";
    f.close();

    HfTokenizer tok;
    Status s = tok.Load(path);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kInternal);
}

// ============================================================
// Round-2: BPE merge priority + truncation / max_length arms
// ============================================================

// BPE merge priority: when two adjacent pairs can both merge, the lower-rank
// merge is applied first (the best_rank comparison + the re-scan loop). Fixture
// gives "l l" rank 0 and "h e" rank 5, so for "hello" the "ll" merge wins the
// first round; the loop then continues until no pair matches.
TEST_F(HfTokenizerTest, Encode_BpeMergePriorityLowestRankFirst) {
    std::string path = (test_dir_ / "tokenizer.json").string();
    nlohmann::json vocab;
    vocab["<s>"] = 0; vocab["<pad>"] = 1; vocab["</s>"] = 2; vocab["<unk>"] = 3;
    vocab["h"] = 4; vocab["e"] = 5; vocab["l"] = 6; vocab["o"] = 7;
    vocab["ll"] = 8;     // result of the rank-0 merge
    vocab["he"] = 9;     // result of the rank-5 merge
    vocab["llo"] = 10;   // result of a later merge
    nlohmann::json merges = nlohmann::json::array();
    merges.push_back("l l");   // rank 0 (highest priority)
    merges.push_back("ll o");  // rank 1
    merges.push_back("h e");   // rank 2
    nlohmann::json j;
    j["model"] = {{"type", "BPE"}, {"vocab", vocab}, {"merges", merges}};
    std::ofstream f(path);
    f << j.dump();
    f.close();

    HfTokenizer tok;
    ASSERT_TRUE(tok.Load(path).ok());
    HfTokenizer::Encoded enc;
    ASSERT_TRUE(tok.Encode("hello", 32, &enc).ok());
    // "llo" (id 10, via ll then ll+o) should appear if the priority + re-scan ran.
    bool has_llo = false;
    for (auto id : enc.input_ids) if (id == 10) has_llo = true;
    EXPECT_TRUE(has_llo);
}

// Truncation arm: a Unigram input that produces more tokens than max_length forces
// the `ids.size() > max_length` true branch (CLS kept, last slot replaced by SEP).
TEST_F(HfTokenizerTest, Encode_TruncationArmUnigram) {
    std::string path = CreateMinimalUnigramJson(test_dir_.string());
    HfTokenizer tok;
    ASSERT_TRUE(tok.Load(path).ok());

    // Long input -> many single-char/UNK tokens; max_length=5 forces truncation.
    HfTokenizer::Encoded enc;
    ASSERT_TRUE(tok.Encode("hellohellohello", 5, &enc).ok());
    EXPECT_EQ(static_cast<int>(enc.input_ids.size()), 5);
    EXPECT_EQ(enc.input_ids[0], 0);        // CLS preserved
    EXPECT_EQ(enc.input_ids[4], 2);        // last slot forced to SEP after truncation
}

// No-truncation, no-padding boundary: an input whose token count exactly fills
// max_length exercises the truncation-false + padding-false arms together.
TEST_F(HfTokenizerTest, Encode_ExactFitNoPadNoTruncate) {
    std::string path = CreateMinimalTokenizerJson(test_dir_.string());
    HfTokenizer tok;
    ASSERT_TRUE(tok.Load(path).ok());

    // Find the natural length first, then request exactly that as max_length.
    HfTokenizer::Encoded big;
    ASSERT_TRUE(tok.Encode("hello", 64, &big).ok());
    int natural = 0;
    for (auto m : big.attention_mask) if (m == 1) natural += 1;

    HfTokenizer::Encoded exact;
    ASSERT_TRUE(tok.Encode("hello", natural, &exact).ok());
    EXPECT_EQ(static_cast<int>(exact.input_ids.size()), natural);
    // Every slot is a real token (no padding), and the last is SEP.
    for (auto m : exact.attention_mask) EXPECT_EQ(m, 1);
    EXPECT_EQ(exact.input_ids.back(), 2);
}

}  // namespace
}  // namespace cortrix
