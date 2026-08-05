// Doc summary S4.6 — doc-level FTS5 hybrid fallback index (§4.3 / §8.2). Self-contained
// (:memory: SQLite), BM25 column weights, FTS5 injection guard, and the two-path
// doc-discovery RRF fusion (FuseDocDiscovery).
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "cortrix/doc_summary/doc_fts5_index.h"
#include "cortrix/doc_summary/f41_schema_provider.h"

#include <sqlite3.h>

namespace cortrix::doc_summary {
namespace {

DocFtsRow Row(std::string doc_id, std::string title, std::string topics = "",
              std::string filename = "", std::string authors = "") {
    DocFtsRow r;
    r.doc_id = std::move(doc_id);
    r.doc_title = std::move(title);
    r.topics_rule_extracted = std::move(topics);
    r.filename = std::move(filename);
    r.authors = std::move(authors);
    return r;
}

class F41DocFts5IndexTest : public ::testing::Test {
protected:
    void SetUp() override { ASSERT_TRUE(idx_.Open(":memory:").ok()); }
    DocFts5Index idx_;
};

TEST_F(F41DocFts5IndexTest, OpenCreatesUsableIndex) {
    EXPECT_TRUE(idx_.is_open());
}

TEST_F(F41DocFts5IndexTest, UpsertThenSearchMatchesTitle) {
    ASSERT_TRUE(idx_.Upsert(Row("d1", "Q3 Financial Report", "revenue finance")).ok());
    ASSERT_TRUE(idx_.Upsert(Row("d2", "RAG Architecture Guide", "vector search ann")).ok());

    auto r = idx_.Search("financial", 10);
    ASSERT_TRUE(r.ok()) << r.status().message();
    ASSERT_FALSE(r.value().empty());
    EXPECT_EQ(r.value()[0].doc_id, "d1");
    EXPECT_EQ(r.value()[0].doc_title, "Q3 Financial Report");
    EXPECT_GT(r.value()[0].score, 0.0);
}

TEST_F(F41DocFts5IndexTest, SearchMatchesTopics) {
    ASSERT_TRUE(idx_.Upsert(Row("d1", "Doc One", "machine learning")).ok());
    ASSERT_TRUE(idx_.Upsert(Row("d2", "Doc Two", "distributed systems")).ok());
    auto r = idx_.Search("distributed", 10);
    ASSERT_TRUE(r.ok());
    ASSERT_EQ(r.value().size(), 1u);
    EXPECT_EQ(r.value()[0].doc_id, "d2");
}

// QA 2026-07-12 F-2: SearchDocFts5 shares the block-path sanitizer, so the D6
// OR-join applies to doc discovery too. A natural-language query whose tokens
// do NOT all co-occur in one row must still surface the partial matches —
// the old implicit-AND semantics starved this to zero rows.
TEST_F(F41DocFts5IndexTest, MultiTokenQueryMatchesPartialTerms) {
    ASSERT_TRUE(idx_.Upsert(Row("d1", "Q3 Financial Report", "revenue")).ok());
    ASSERT_TRUE(idx_.Upsert(Row("d2", "Distributed Systems Guide", "consensus")).ok());

    auto r = idx_.Search("financial consensus", 10);
    ASSERT_TRUE(r.ok()) << r.status().message();
    // Minimal repro (QA 2026-07-12 FYI-2): two tokens, each matching a
    // DIFFERENT doc. OR semantics returns both; AND would demand
    // co-occurrence inside one row and return none.
    ASSERT_EQ(r.value().size(), 2u);
}

TEST_F(F41DocFts5IndexTest, UpsertIsIdempotentOnDocId) {
    ASSERT_TRUE(idx_.Upsert(Row("d1", "Old Title", "old")).ok());
    ASSERT_TRUE(idx_.Upsert(Row("d1", "New Title", "new")).ok());  // replace
    auto old_r = idx_.Search("old", 10);
    ASSERT_TRUE(old_r.ok());
    EXPECT_TRUE(old_r.value().empty());  // old content gone
    auto new_r = idx_.Search("new", 10);
    ASSERT_TRUE(new_r.ok());
    ASSERT_EQ(new_r.value().size(), 1u);
    EXPECT_EQ(new_r.value()[0].doc_title, "New Title");
}

TEST_F(F41DocFts5IndexTest, EmptyQueryReturnsNoResults) {
    ASSERT_TRUE(idx_.Upsert(Row("d1", "Something")).ok());
    auto r = idx_.Search("   ", 10);
    ASSERT_TRUE(r.ok());
    EXPECT_TRUE(r.value().empty());
}

TEST_F(F41DocFts5IndexTest, ZeroTopKReturnsEmpty) {
    ASSERT_TRUE(idx_.Upsert(Row("d1", "Something")).ok());
    auto r = idx_.Search("something", 0);
    ASSERT_TRUE(r.ok());
    EXPECT_TRUE(r.value().empty());
}

TEST_F(F41DocFts5IndexTest, Fts5OperatorInjectionIsSanitized) {
    ASSERT_TRUE(idx_.Upsert(Row("d1", "report about systems")).ok());
    // Raw FTS5 operators must not throw a query error (M-SEC-001 sanitize). The
    // call returns ok() with whatever the sanitized query matched (possibly empty).
    auto r = idx_.Search("report OR (systems NEAR", 10);
    EXPECT_TRUE(r.ok()) << r.status().message();
}

TEST(F41DocFts5IndexLifecycleTest, SearchOnClosedIndexErrors) {
    DocFts5Index idx;  // not opened
    auto r = idx.Search("x", 5);
    EXPECT_FALSE(r.ok());
    EXPECT_NE(r.status().message().find("CX_ERR_DOCSUMMARY_FTS5_FALLBACK_FAILED"),
              std::string::npos);
}

TEST(F41DocFts5IndexLifecycleTest, UpsertOnClosedIndexErrors) {
    DocFts5Index idx;
    auto st = idx.Upsert(Row("d1", "t"));
    EXPECT_FALSE(st.ok());
}

TEST(F41DocFts5IndexLifecycleTest, OpenOnUncreatablePathErrors) {
    // A path under a non-existent directory cannot be created → sqlite3_open
    // (with the default create flags) fails → CX_ERR_DOCSUMMARY_FTS5_FALLBACK_FAILED.
    DocFts5Index idx;
    Status st = idx.Open("/nonexistent_dir_f41_xyz/sub/doc.sqlite");
    EXPECT_FALSE(st.ok());
    EXPECT_NE(st.message().find("CX_ERR_DOCSUMMARY_FTS5_FALLBACK_FAILED"),
              std::string::npos);
    EXPECT_FALSE(idx.is_open());
}

TEST(F41DocFts5SharedHandleTest, BorrowedHandleUpsertSearchDelete) {
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(db, kDocFts5IndexDdl, nullptr, nullptr, nullptr), SQLITE_OK);

    ASSERT_TRUE(UpsertDocFts5Row(db, Row("d1", "Shared Handle", "rollbacktoken")).ok());
    auto before = SearchDocFts5(db, "rollbacktoken", 10);
    ASSERT_TRUE(before.ok()) << before.status().message();
    ASSERT_EQ(before.value().size(), 1u);
    EXPECT_EQ(before.value()[0].doc_id, "d1");

    ASSERT_TRUE(DeleteDocFts5Row(db, "d1").ok());
    ASSERT_TRUE(DeleteDocFts5Row(db, "d1").ok()) << "delete must be idempotent";
    auto after = SearchDocFts5(db, "rollbacktoken", 10);
    ASSERT_TRUE(after.ok()) << after.status().message();
    EXPECT_TRUE(after.value().empty());

    sqlite3_close(db);
}

TEST(F41DocFts5SharedHandleTest, BorrowedHandleErrorsWithoutMigratedTable) {
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);

    Status st = UpsertDocFts5Row(db, Row("d1", "Missing Table"));
    EXPECT_FALSE(st.ok());
    EXPECT_NE(st.message().find("CX_ERR_DOCSUMMARY_FTS5_FALLBACK_FAILED"),
              std::string::npos);

    sqlite3_close(db);
}

// ---------- FuseDocDiscovery (two-path RRF, §8.2) ----------

DocDiscoveryHit LlmHit(std::string doc_id, std::string summary) {
    DocDiscoveryHit h;
    h.doc_id = std::move(doc_id);
    h.via_path = "llm_summary";
    h.summary_text = std::move(summary);
    return h;
}

DocFtsHit FtsHit(std::string doc_id, std::string title, double score) {
    DocFtsHit h;
    h.doc_id = std::move(doc_id);
    h.doc_title = std::move(title);
    h.score = score;
    return h;
}

TEST(F41FuseDocDiscoveryTest, MergesBothPathsAndDedups) {
    std::vector<DocDiscoveryHit> llm = {LlmHit("d1", "s1"), LlmHit("d2", "s2")};
    std::vector<DocFtsHit> fts = {FtsHit("d2", "t2", 0.9), FtsHit("d3", "t3", 0.5)};
    auto out = FuseDocDiscovery(llm, fts, /*top_k=*/10);

    // 3 distinct docs (d1/d2/d3); d2 appears in both → single entry, summed score.
    ASSERT_EQ(out.size(), 3u);
    // d2 is in both paths (rank0 llm + rank0 fts) → highest fused score → first.
    EXPECT_EQ(out[0].doc_id, "d2");
    EXPECT_EQ(out[0].via_path, "llm_summary");  // llm metadata wins on collision
}

TEST(F41FuseDocDiscoveryTest, Fts5OnlyDocGetsFts5ViaPath) {
    std::vector<DocDiscoveryHit> llm = {LlmHit("d1", "s1")};
    std::vector<DocFtsHit> fts = {FtsHit("d2", "t2", 0.9)};
    auto out = FuseDocDiscovery(llm, fts, 10);
    ASSERT_EQ(out.size(), 2u);
    bool found_d2 = false;
    for (const auto& h : out) {
        if (h.doc_id == "d2") {
            found_d2 = true;
            EXPECT_EQ(h.via_path, "fts5_fallback");
            EXPECT_EQ(h.doc_title, "t2");
        }
    }
    EXPECT_TRUE(found_d2);
}

TEST(F41FuseDocDiscoveryTest, TopKTruncates) {
    std::vector<DocDiscoveryHit> llm = {LlmHit("d1", "s1"), LlmHit("d2", "s2"),
                                        LlmHit("d3", "s3")};
    auto out = FuseDocDiscovery(llm, {}, /*top_k=*/2);
    EXPECT_EQ(out.size(), 2u);
}

TEST(F41FuseDocDiscoveryTest, RankOrderPreservedByRrf) {
    // Two summary hits, no fts. RRF gives rank0 > rank1, so order is preserved.
    std::vector<DocDiscoveryHit> llm = {LlmHit("dA", "a"), LlmHit("dB", "b")};
    auto out = FuseDocDiscovery(llm, {}, 10);
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0].doc_id, "dA");
    EXPECT_GT(out[0].match_score, out[1].match_score);
}

TEST(F41FuseDocDiscoveryTest, EmptyInputsGiveEmpty) {
    auto out = FuseDocDiscovery({}, {}, 10);
    EXPECT_TRUE(out.empty());
}

}  // namespace
}  // namespace cortrix::doc_summary
