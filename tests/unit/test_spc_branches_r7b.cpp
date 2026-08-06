// R7-1d branch-coverage supplement for three spc-side files whose reachable
// error/boundary arms the existing tests don't hit:
//   - contextual_schema_provider.cpp  : the null-db guard (line 58);
//   - contextual_store.cpp     : null-db (19), the embedding-present + status=failed
//                                combo (44-67), and the no-contextual-output early-Ok (28);
//   - wordpiece_tokenizer.cpp  : the UTF-8 decode error arms (invalid lead byte /
//                                truncated / bad continuation — reached via the
//                                PUBLIC BasicTokenize, since CleanText/NextCodePoint
//                                are private), the empty-word WordPieceEncode arm,
//                                and the LoadVocab / Encode guard arms.
//
// NOTE on wordpiece access: CleanText / PadChineseChars / NextCodePoint are PRIVATE
// static members (wordpiece_tokenizer.h:95-118). The only public string entry points
// are BasicTokenize() + WordPieceEncode(), so the malformed-UTF-8 byte sequences are
// fed through BasicTokenize (it calls CleanText → NextCodePoint), which is what hits
// the 0xFFFD return arms. (The recon's "call CleanText directly" would not compile.)
//
// Unreachable / ERROR-INJECTION-ONLY arms left uncovered ON PURPOSE (a healthy
// memory: DB never fails prepare/step;#4 — do not force):
//   - contextual/contextual_store: the sqlite3_prepare / sqlite3_step != OK error returns.
//
// Standalone NEW file; touches no existing test.
#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include <sqlite3.h>

#include "cortrix/spc_enricher/contextual_schema_provider.h"
#include "cortrix/spc/contextual_store.h"
#include "cortrix/spc_enricher.h"  // EnrichResult
#include "cortrix/query/wordpiece_tokenizer.h"

namespace cortrix {
namespace {

// ============================================================
// contextual_schema_provider.cpp — null-db guard.
// ============================================================

// Migrate(nullptr, 0, 1): the init pair passes the version check, then the !db
// guard (line 58) fires → InvalidArgument "contextual migrate: null db". The existing
// contextual test never passes a null db.
TEST(ContextualSchemaProviderBranchR7b, NullDbInvalidArgument) {
    spc::ContextualSchemaProvider p;
    Status st = p.Migrate(nullptr, 0, 1);
    EXPECT_FALSE(st.ok());
    EXPECT_NE(st.message().find("null db"), std::string::npos);
}

// A same-version step (within CurrentVersion) with a null db still hits the
// null-db arm: the forward-only guard tolerates from==to==CurrentVersion, so
// execution proceeds to the !db check. (Beyond-current pairs like (3,3) are now
// rejected by the version guard first — covered by the mismatch tests.)
TEST(ContextualSchemaProviderBranchR7b, NullDbSameVersionNonInit) {
    spc::ContextualSchemaProvider p;
    Status st = p.Migrate(nullptr, 2, 2);
    EXPECT_FALSE(st.ok());
    EXPECT_NE(st.message().find("null db"), std::string::npos);
}

// ============================================================
// contextual_store.cpp — WriteContextualized reachable arms.
// ============================================================

// A `blocks` table shaped like the contextual retrieval migration leaves it (the columns
// WriteContextualized updates). One seed row so the UPDATE matches.
void CreateBlocksForContextual(sqlite3* db) {
    ASSERT_EQ(sqlite3_exec(db,
        "CREATE TABLE blocks (block_id INTEGER PRIMARY KEY, "
        "contextualized_text TEXT, contextualized_embedding BLOB, "
        "contextualized_status INTEGER DEFAULT 0)",
        nullptr, nullptr, nullptr), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(db,
        "INSERT INTO blocks(block_id) VALUES (7)", nullptr, nullptr, nullptr), SQLITE_OK);
}

// Null db → the line-19 guard returns InvalidArgument before any SQL.
TEST(ContextualStoreBranchR7b, NullDbInvalidArgument) {
    spc::EnrichResult r;
    r.contextualized_status = 1;  // would otherwise proceed
    Status st = spc::WriteContextualized(nullptr, 7, r);
    EXPECT_FALSE(st.ok());
    EXPECT_NE(st.message().find("null db"), std::string::npos);
}

// No contextual retrieval output (status 0, no optionals) → the !contextual_ran early-Ok (line 28); the
// row is left untouched (status stays its DEFAULT 0).
TEST(ContextualStoreBranchR7b, NoContextualOutputEarlyOk) {
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    CreateBlocksForContextual(db);
    spc::EnrichResult r;  // status 0, both optionals empty → contextual_ran == false
    EXPECT_TRUE(spc::WriteContextualized(db, 7, r).ok());
    sqlite3_close(db);
}

// Embedding present + status=failed(2): contextual_ran is true (status != 0), the text
// optional is ABSENT (bind_null arm, line 47), the embedding optional is PRESENT
// and non-empty (bind_blob arm, line 56). Covers the has_value() false-for-text /
// true-for-embedding combination the existing test doesn't reach together.
TEST(ContextualStoreBranchR7b, EmbeddingPresentTextAbsentStatusFailed) {
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    CreateBlocksForContextual(db);

    spc::EnrichResult r;
    r.contextualized_status = 2;  // failed → contextual_ran true even with no text
    r.contextualized_embedding = std::vector<float>{0.5f, 0.25f};  // present + non-empty
    // contextualized_text intentionally left unset → bind_null arm.
    EXPECT_TRUE(spc::WriteContextualized(db, 7, r).ok());

    // Verify the embedding BLOB + status landed; text stayed NULL.
    sqlite3_stmt* st = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(db,
        "SELECT contextualized_text, length(contextualized_embedding), "
        "contextualized_status FROM blocks WHERE block_id=7", -1, &st, nullptr), SQLITE_OK);
    ASSERT_EQ(sqlite3_step(st), SQLITE_ROW);
    EXPECT_EQ(sqlite3_column_type(st, 0), SQLITE_NULL);          // text bound null
    EXPECT_EQ(sqlite3_column_int(st, 1), 2 * static_cast<int>(sizeof(float)));  // 2 floats
    EXPECT_EQ(sqlite3_column_int(st, 2), 2);                      // status=failed
    sqlite3_finalize(st);
    sqlite3_close(db);
}

// Text present + embedding ABSENT (and an empty embedding vector also routes to
// bind_null via the `!empty()` guard): covers the text bind_text arm (45) + the
// embedding bind_null arm (59) — the mirror of the test above.
TEST(ContextualStoreBranchR7b, TextPresentEmbeddingAbsent) {
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    CreateBlocksForContextual(db);

    spc::EnrichResult r;
    r.contextualized_text = "ctx text";        // present → bind_text
    r.contextualized_embedding = std::vector<float>{};  // present-but-EMPTY → bind_null
    r.contextualized_status = 1;
    EXPECT_TRUE(spc::WriteContextualized(db, 7, r).ok());

    sqlite3_stmt* st = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(db,
        "SELECT contextualized_text, contextualized_embedding FROM blocks WHERE block_id=7",
        -1, &st, nullptr), SQLITE_OK);
    ASSERT_EQ(sqlite3_step(st), SQLITE_ROW);
    EXPECT_STREQ(reinterpret_cast<const char*>(sqlite3_column_text(st, 0)), "ctx text");
    EXPECT_EQ(sqlite3_column_type(st, 1), SQLITE_NULL);  // empty vector → null blob
    sqlite3_finalize(st);
    sqlite3_close(db);
}

// contextual_ran driven by the contextualized_text optional alone (status 0, no embedding):
// the `result.contextualized_text.has_value()` disjunct of the contextual_ran test (line
// 26) — distinct from the status!=0 trigger above.
TEST(ContextualStoreBranchR7b, ContextualRanViaTextOptionalOnly) {
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    CreateBlocksForContextual(db);

    spc::EnrichResult r;
    r.contextualized_text = "only text, status 0";  // engages contextual_ran via has_value()
    EXPECT_TRUE(spc::WriteContextualized(db, 7, r).ok());
    sqlite3_close(db);
}

// ============================================================
// wordpiece_tokenizer.cpp — UTF-8 decode error arms (via public BasicTokenize)
// + WordPieceEncode empty-word + LoadVocab / Encode guards.
// ============================================================

using cortrix::query::WordPieceTokenizer;

// Write a throwaway vocab.txt with the given lines; returns its path (caller
// removes). Used to drive LoadVocab's branches.
static std::string WriteVocab(const std::vector<std::string>& lines) {
    char tmpl[] = "/tmp/cortrix_wpvocab_XXXXXX";
    int fd = ::mkstemp(tmpl);
    if (fd >= 0) ::close(fd);
    std::string path = std::string(tmpl) + ".txt";
    std::rename(tmpl, path.c_str());
    std::ofstream f(path);
    for (const auto& l : lines) f << l << "\n";
    f.close();
    return path;
}

// A complete minimal vocab with the 4 required special tokens + a couple of words.
static std::string WriteGoodVocab() {
    return WriteVocab({"[PAD]", "[UNK]", "[CLS]", "[SEP]", "hello", "world"});
}

// Invalid UTF-8 lead byte (0xFF) → NextCodePoint line 102 returns 0xFFFD → CleanText
// strips it. Reached through the public BasicTokenize. The byte produces no word.
TEST(WordPieceBranchR7b, BasicTokenizeInvalidLeadByteStripped) {
    WordPieceTokenizer t;
    auto words = t.BasicTokenize("\xFF");
    EXPECT_TRUE(words.empty());  // 0xFFFD stripped, nothing emitted
}

// Truncated UTF-8 (a lone 0xC3 lead byte with no continuation) → line 103
// (i+len > n) → 0xFFFD → stripped.
TEST(WordPieceBranchR7b, BasicTokenizeTruncatedUtf8Stripped) {
    WordPieceTokenizer t;
    auto words = t.BasicTokenize("\xC3");
    EXPECT_TRUE(words.empty());
}

// Bad continuation byte (0xC3 followed by 0x28 which is not 10xxxxxx) → line 106
// (bad continuation) → 0xFFFD for the lead; the 0x28 '(' is ASCII punctuation and
// survives as its own token.
TEST(WordPieceBranchR7b, BasicTokenizeBadContinuationByte) {
    WordPieceTokenizer t;
    auto words = t.BasicTokenize("\xC3\x28");
    // The 0xFFFD from the bad sequence is stripped; '(' is a punctuation token.
    ASSERT_EQ(words.size(), 1u);
    EXPECT_EQ(words[0], "(");
}

// WordPieceEncode("") → line 319 char_count==0 → returns an empty id vector (no
// [UNK]). Distinct from the over-long-word [UNK] arm.
TEST(WordPieceBranchR7b, WordPieceEncodeEmptyWordReturnsEmpty) {
    WordPieceTokenizer t;
    EXPECT_TRUE(t.WordPieceEncode("").empty());
}

// A word longer than kMaxInputCharsPerWord (100) → line 320-322 → a single [UNK].
// unk_id defaults to 100 when no vocab loaded (header default), so the id is 100.
TEST(WordPieceBranchR7b, WordPieceEncodeOverlongWordIsUnk) {
    WordPieceTokenizer t;
    std::string huge(101, 'a');
    auto ids = t.WordPieceEncode(huge);
    ASSERT_EQ(ids.size(), 1u);
    EXPECT_EQ(ids[0], 100);  // unk_id_ default
}

// LoadVocab on a missing file → NotFound (line 374-375).
TEST(WordPieceBranchR7b, LoadVocabMissingFileNotFound) {
    WordPieceTokenizer t;
    Status st = t.LoadVocab("/nonexistent/vocab.txt");
    EXPECT_FALSE(st.ok());
    EXPECT_NE(st.message().find("not found"), std::string::npos);
    EXPECT_FALSE(t.loaded());
}

// LoadVocab on a file that yields an EMPTY vocab (only a blank line that, after the
// loop, leaves vocab_ with one empty-string entry — but the design treats a file
// with no usable tokens). A truly empty file → vocab_.empty() → line 386-387
// Internal "empty vocab". Use a zero-byte file.
TEST(WordPieceBranchR7b, LoadVocabEmptyFileIsInternalError) {
    char tmpl[] = "/tmp/cortrix_wpempty_XXXXXX";
    int fd = ::mkstemp(tmpl);
    if (fd >= 0) ::close(fd);
    std::string path = std::string(tmpl) + ".txt";
    std::rename(tmpl, path.c_str());
    { std::ofstream f(path); }  // create truly empty file

    WordPieceTokenizer t;
    Status st = t.LoadVocab(path);
    EXPECT_FALSE(st.ok());
    EXPECT_NE(st.message().find("empty vocab"), std::string::npos);
    EXPECT_FALSE(t.loaded());
    std::remove(path.c_str());
}

// LoadVocab on a vocab MISSING a required special token ([SEP] absent) → line
// 396-400 Internal "missing a required special token".
TEST(WordPieceBranchR7b, LoadVocabMissingSpecialTokenIsError) {
    std::string path = WriteVocab({"[PAD]", "[UNK]", "[CLS]", "hello"});  // no [SEP]
    WordPieceTokenizer t;
    Status st = t.LoadVocab(path);
    EXPECT_FALSE(st.ok());
    EXPECT_NE(st.message().find("required special token"), std::string::npos);
    EXPECT_FALSE(t.loaded());
    std::remove(path.c_str());
}

// A good vocab loads cleanly (the success path through LoadVocab's resolve lambda
// for all 4 tokens) — anchors the failure tests above + flips loaded().
TEST(WordPieceBranchR7b, LoadVocabGoodSucceeds) {
    std::string path = WriteGoodVocab();
    WordPieceTokenizer t;
    Status st = t.LoadVocab(path);
    EXPECT_TRUE(st.ok()) << st.message();
    EXPECT_TRUE(t.loaded());
    EXPECT_EQ(t.cls_id(), 2);   // line index of [CLS]
    EXPECT_EQ(t.sep_id(), 3);
    std::remove(path.c_str());
}

// Encode with a null output → line 409-410 InvalidArgument "null output".
TEST(WordPieceBranchR7b, EncodeNullOutputInvalidArgument) {
    std::string path = WriteGoodVocab();
    WordPieceTokenizer t;
    ASSERT_TRUE(t.LoadVocab(path).ok());
    Status st = t.Encode("text", 64, nullptr);
    EXPECT_FALSE(st.ok());
    EXPECT_NE(st.message().find("null output"), std::string::npos);
    std::remove(path.c_str());
}

// Encode before LoadVocab → line 412-413 Internal "vocab not loaded".
TEST(WordPieceBranchR7b, EncodeBeforeLoadVocabIsError) {
    WordPieceTokenizer t;
    WordPieceTokenizer::Encoded enc;
    Status st = t.Encode("text", 64, &enc);
    EXPECT_FALSE(st.ok());
    EXPECT_NE(st.message().find("not loaded"), std::string::npos);
}

// Encode with max_length < 2 → line 415-416 InvalidArgument "max_length must be >= 2".
TEST(WordPieceBranchR7b, EncodeMaxLengthTooSmall) {
    std::string path = WriteGoodVocab();
    WordPieceTokenizer t;
    ASSERT_TRUE(t.LoadVocab(path).ok());
    WordPieceTokenizer::Encoded enc;
    Status st = t.Encode("text", 1, &enc);
    EXPECT_FALSE(st.ok());
    EXPECT_NE(st.message().find("max_length"), std::string::npos);
    std::remove(path.c_str());
}

// A happy Encode (truncation budget exercised): max_length=2 leaves budget 0, so
// only [CLS][SEP] survive (the `content.size() >= budget` break arm, line 429).
TEST(WordPieceBranchR7b, EncodeBudgetZeroOnlySpecials) {
    std::string path = WriteGoodVocab();
    WordPieceTokenizer t;
    ASSERT_TRUE(t.LoadVocab(path).ok());
    WordPieceTokenizer::Encoded enc;
    ASSERT_TRUE(t.Encode("hello world", 2, &enc).ok());
    ASSERT_EQ(enc.input_ids.size(), 2u);   // [CLS] [SEP] only (budget 0)
    EXPECT_EQ(enc.input_ids.front(), t.cls_id());
    EXPECT_EQ(enc.input_ids.back(), t.sep_id());
    EXPECT_EQ(enc.attention_mask.size(), 2u);
    std::remove(path.c_str());
}

}  // namespace
}  // namespace cortrix
