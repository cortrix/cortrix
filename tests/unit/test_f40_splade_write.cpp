#include <gtest/gtest.h>

#include <sqlite3.h>

#include <cstdio>
#include <cstdint>
#include <map>
#include <memory>
#include <string>

#include "cortrix/retrieval/splade_sparse_retriever.h"

// Sparse retrieval S5 — SpladeSparseRetriever write/delete (§6.1, §6.2): inverted-index DDL
// (Open), incremental Add (replace semantics), Remove (idempotent). Verified by
// querying the underlying table directly + via the retriever's own read where
// useful. Runs against an in-memory SQLite DB (standalone — write coordinator PWL / parent-child chunking
// children FK = D3.5).
namespace cortrix::retrieval {
namespace {

SparseVector V(std::map<uint32_t, float> t) {
    SparseVector v;
    v.terms = std::move(t);
    return v;
}

// Count postings for (ns, child) by reopening a 2nd read connection on the same
// file — for :memory: we instead expose count via a query helper on the same db.
// Simplest: count via the retriever's Search isn't direct; use a small fixture
// that owns its own verification by re-querying with a temp file DB.
class SpladeWriteTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Temp file DB so a 2nd connection can verify row counts independently.
        db_path_ = std::string(testing::TempDir()) + "/f40_splade_" +
                   std::to_string(reinterpret_cast<uintptr_t>(this)) + ".db";
        std::remove(db_path_.c_str());
        r_ = std::make_unique<SpladeSparseRetriever>(SpladeConfig{}, db_path_);
        ASSERT_TRUE(r_->Open().ok());
    }
    void TearDown() override {
        r_.reset();
        std::remove(db_path_.c_str());
    }

    int CountPostings(const std::string& ns, const std::string& child) {
        sqlite3* db = nullptr;
        EXPECT_EQ(sqlite3_open(db_path_.c_str(), &db), SQLITE_OK);
        sqlite3_stmt* st = nullptr;
        sqlite3_prepare_v2(db,
            "SELECT COUNT(*) FROM sparse_inverted_index WHERE ns_id=? AND child_id=?",
            -1, &st, nullptr);
        sqlite3_bind_text(st, 1, ns.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, child.c_str(), -1, SQLITE_TRANSIENT);
        int n = (sqlite3_step(st) == SQLITE_ROW) ? sqlite3_column_int(st, 0) : -1;
        sqlite3_finalize(st);
        sqlite3_close(db);
        return n;
    }

    int CountTotal() {
        sqlite3* db = nullptr;
        EXPECT_EQ(sqlite3_open(db_path_.c_str(), &db), SQLITE_OK);
        sqlite3_stmt* st = nullptr;
        sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM sparse_inverted_index",
                           -1, &st, nullptr);
        int n = (sqlite3_step(st) == SQLITE_ROW) ? sqlite3_column_int(st, 0) : -1;
        sqlite3_finalize(st);
        sqlite3_close(db);
        return n;
    }

    std::string db_path_;
    std::unique_ptr<SpladeSparseRetriever> r_;
};

TEST_F(SpladeWriteTest, OpenIsIdempotent) {
    EXPECT_TRUE(r_->Open().ok());  // 2nd Open → ok
    EXPECT_TRUE(r_->IsAvailable());
}

TEST_F(SpladeWriteTest, AddWritesOnePostingPerTerm) {
    ASSERT_TRUE(r_->Add("ns", "c1", V({{1, 0.9f}, {2, 0.5f}, {3, 0.1f}})).ok());
    EXPECT_EQ(CountPostings("ns", "c1"), 3);
}

TEST_F(SpladeWriteTest, AddEmptyVectorWritesNothing) {
    ASSERT_TRUE(r_->Add("ns", "c1", V({})).ok());  // dead chunk (§6.5)
    EXPECT_EQ(CountPostings("ns", "c1"), 0);
    EXPECT_EQ(CountTotal(), 0);
}

TEST_F(SpladeWriteTest, ReAddReplacesPostings) {
    ASSERT_TRUE(r_->Add("ns", "c1", V({{1, 0.9f}, {2, 0.5f}})).ok());
    EXPECT_EQ(CountPostings("ns", "c1"), 2);
    // Re-add with a different term set → old postings replaced, not merged.
    ASSERT_TRUE(r_->Add("ns", "c1", V({{5, 0.7f}})).ok());
    EXPECT_EQ(CountPostings("ns", "c1"), 1);
    // And the new term is queryable, old terms gone.
    auto hits5 = r_->Search(V({{5, 1.0f}}), "ns", 10);
    ASSERT_EQ(hits5.size(), 1u);
    EXPECT_TRUE(r_->Search(V({{1, 1.0f}}), "ns", 10).empty());
}

TEST_F(SpladeWriteTest, ReAddToEmptyRemovesChild) {
    ASSERT_TRUE(r_->Add("ns", "c1", V({{1, 0.9f}})).ok());
    ASSERT_TRUE(r_->Add("ns", "c1", V({})).ok());  // now empty → drop
    EXPECT_EQ(CountPostings("ns", "c1"), 0);
}

TEST_F(SpladeWriteTest, RemoveDeletesAllPostings) {
    ASSERT_TRUE(r_->Add("ns", "c1", V({{1, 0.9f}, {2, 0.5f}, {3, 0.1f}})).ok());
    ASSERT_TRUE(r_->Remove("ns", "c1").ok());
    EXPECT_EQ(CountPostings("ns", "c1"), 0);
}

TEST_F(SpladeWriteTest, RemoveIsIdempotent) {
    EXPECT_TRUE(r_->Remove("ns", "absent").ok());  // no-op success
    ASSERT_TRUE(r_->Add("ns", "c1", V({{1, 0.9f}})).ok());
    EXPECT_TRUE(r_->Remove("ns", "c1").ok());
    EXPECT_TRUE(r_->Remove("ns", "c1").ok());  // again → ok
    EXPECT_EQ(CountPostings("ns", "c1"), 0);
}

TEST_F(SpladeWriteTest, RemoveOnlyAffectsTargetChild) {
    ASSERT_TRUE(r_->Add("ns", "c1", V({{1, 0.9f}})).ok());
    ASSERT_TRUE(r_->Add("ns", "c2", V({{1, 0.4f}})).ok());
    ASSERT_TRUE(r_->Remove("ns", "c1").ok());
    EXPECT_EQ(CountPostings("ns", "c1"), 0);
    EXPECT_EQ(CountPostings("ns", "c2"), 1);  // sibling untouched
}

TEST_F(SpladeWriteTest, NamespaceIsolationOnWrite) {
    ASSERT_TRUE(r_->Add("ns_a", "c", V({{1, 0.9f}})).ok());
    ASSERT_TRUE(r_->Add("ns_b", "c", V({{1, 0.4f}})).ok());
    EXPECT_EQ(CountTotal(), 2);
    ASSERT_TRUE(r_->Remove("ns_a", "c").ok());
    EXPECT_EQ(CountPostings("ns_a", "c"), 0);
    EXPECT_EQ(CountPostings("ns_b", "c"), 1);  // other NS unaffected
}

TEST_F(SpladeWriteTest, AddBeforeOpenFails) {
    SpladeSparseRetriever closed(SpladeConfig{}, ":memory:");  // no Open()
    Status s = closed.Add("ns", "c", V({{1, 0.5f}}));
    EXPECT_FALSE(s.ok());
    EXPECT_NE(s.message().find("CX_ERR_F40_INVERTED_INDEX_WRITE_FAILED"),
              std::string::npos);
}

TEST(SpladeWriteMemTest, InMemoryDbWorks) {
    // :memory: path also works (single-connection lifecycle).
    SpladeSparseRetriever r(SpladeConfig{}, ":memory:");
    ASSERT_TRUE(r.Open().ok());
    ASSERT_TRUE(r.Add("ns", "c", V({{1, 0.9f}, {2, 0.3f}})).ok());
    auto hits = r.Search(V({{1, 1.0f}}), "ns", 10);
    ASSERT_EQ(hits.size(), 1u);
    EXPECT_EQ(hits[0].child_id, "c");
}

}  // namespace
}  // namespace cortrix::retrieval
