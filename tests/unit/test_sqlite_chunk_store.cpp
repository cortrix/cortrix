#include "cortrix/store/sqlite_chunk_store.h"

#include <gtest/gtest.h>

#include <sqlite3.h>

#include <string>
#include <vector>

#include "cortrix/store/f34_schema_provider.h"
#include "cortrix/store/per_unit_schema_ddl.h"

namespace cortrix::store {
namespace {

// Standalone fixture: the real per-Unit framework schema (documents/blocks/
// blocks_fts) + the F34 child-column ALTERs (child_id/parent_id/chunk_index live
// on blocks under A unified blocks). Rows are inserted with raw SQL so the test
// needs no F25 PWL / BlockAssembler wiring (mirrors test_scoring_store fixture).
class SqliteChunkStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_EQ(sqlite3_open(":memory:", &db_), SQLITE_OK);
        ASSERT_EQ(sqlite3_exec(db_, kPerUnitFrameworkDdl, nullptr, nullptr, nullptr),
                  SQLITE_OK);
        F34SchemaProvider f34;
        ASSERT_TRUE(f34.Migrate(db_, 0, 1).ok());
    }
    void TearDown() override { sqlite3_close(db_); }

    void Exec(const std::string& sql) {
        char* err = nullptr;
        int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err);
        std::string m = err ? err : "";
        sqlite3_free(err);
        ASSERT_EQ(rc, SQLITE_OK) << m << " :: " << sql;
    }

    // A child block row (child_id NOT NULL → a child chunk). parent empty → NULL.
    void InsertChild(int64_t bid, const std::string& doc, int idx,
                     const std::string& child, const std::string& parent,
                     const std::string& text) {
        const std::string p = parent.empty() ? "NULL" : ("'" + parent + "'");
        Exec("INSERT INTO blocks(block_id,doc_id,chunk_index,block_type,"
             "processing_level,data,content_text,child_id,parent_id) VALUES(" +
             std::to_string(bid) + ",'" + doc + "'," + std::to_string(idx) +
             ",1,3,X'00','" + text + "','" + child + "'," + p + ")");
    }

    // A non-child row (F08 META, block_type=8, child_id NULL) — must be excluded
    // by GetChunksByDocId (child_id IS NOT NULL filter).
    void InsertMeta(int64_t bid, const std::string& doc) {
        Exec("INSERT INTO blocks(block_id,doc_id,chunk_index,block_type,"
             "processing_level,data,content_text,child_id) VALUES(" +
             std::to_string(bid) + ",'" + doc + "',0,8,3,X'00','docmeta',NULL)");
    }

    sqlite3* db_ = nullptr;
};

TEST_F(SqliteChunkStoreTest, GetReturnsChildRecord) {
    InsertChild(101, "d1", 0, "c1", "p1", "hello world");
    SqliteChunkStore store(db_);
    auto r = store.Get("c1");
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r->child_id, "c1");
    EXPECT_EQ(r->parent_id, "p1");
    EXPECT_EQ(r->content, "hello world");
    EXPECT_EQ(r->chunk_index, 0);
}

TEST_F(SqliteChunkStoreTest, GetMissingReturnsStoreNotFound) {
    SqliteChunkStore store(db_);
    auto r = store.Get("nope");
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.status().code(), StatusCode::kNotFound);
    EXPECT_NE(r.status().message().find("CX_ERR_STORE_NOT_FOUND"), std::string::npos);
}

TEST_F(SqliteChunkStoreTest, GetFlatChildHasEmptyParent) {
    InsertChild(102, "d1", 0, "c1", "", "flat chunk");  // parent_id NULL
    SqliteChunkStore store(db_);
    auto r = store.Get("c1");
    ASSERT_TRUE(r.ok());
    EXPECT_TRUE(r->parent_id.empty());
}

TEST_F(SqliteChunkStoreTest, GetBatchPreservesOrderAndCollectsMissing) {
    InsertChild(201, "d1", 0, "c1", "p1", "one");
    InsertChild(202, "d1", 1, "c2", "p1", "two");
    InsertChild(203, "d1", 2, "c3", "p1", "three");
    SqliteChunkStore store(db_);
    std::vector<std::string> missing;
    auto recs = store.GetBatch({"c2", "missingX", "c1"}, &missing);
    ASSERT_EQ(recs.size(), 2u);
    EXPECT_EQ(recs[0].child_id, "c2");   // output order matches input order
    EXPECT_EQ(recs[1].child_id, "c1");
    ASSERT_EQ(missing.size(), 1u);
    EXPECT_EQ(missing[0], "missingX");
}

TEST_F(SqliteChunkStoreTest, GetBatchNullMissingPtrIsSafe) {
    InsertChild(204, "d1", 0, "c1", "p1", "one");
    SqliteChunkStore store(db_);
    auto recs = store.GetBatch({"c1", "absent"}, nullptr);  // no missing collector
    ASSERT_EQ(recs.size(), 1u);
    EXPECT_EQ(recs[0].child_id, "c1");
}

TEST_F(SqliteChunkStoreTest, ReadsOptionalScoreSignalsWhenColumnsExist) {
    Exec("ALTER TABLE blocks ADD COLUMN enriched_score REAL DEFAULT NULL");
    Exec("ALTER TABLE blocks ADD COLUMN semantic_score REAL DEFAULT NULL");
    InsertChild(205, "d1", 0, "c1", "p1", "one");
    InsertChild(206, "d1", 1, "c2", "p1", "two");
    Exec("UPDATE blocks SET enriched_score=0.8, semantic_score=1.0 WHERE child_id='c1'");
    Exec("UPDATE blocks SET semantic_score=0.6 WHERE child_id='c2'");

    SqliteChunkStore store(db_);
    auto one = store.Get("c1");
    ASSERT_TRUE(one.ok());
    ASSERT_TRUE(one->score_signals.enriched_score.has_value());
    ASSERT_TRUE(one->score_signals.semantic_score.has_value());
    EXPECT_FLOAT_EQ(*one->score_signals.enriched_score, 0.8f);
    EXPECT_FLOAT_EQ(*one->score_signals.semantic_score, 1.0f);

    std::vector<std::string> missing;
    auto batch = store.GetBatch({"c2", "c1"}, &missing);
    ASSERT_TRUE(missing.empty());
    ASSERT_EQ(batch.size(), 2u);
    EXPECT_FALSE(batch[0].score_signals.enriched_score.has_value());
    ASSERT_TRUE(batch[0].score_signals.semantic_score.has_value());
    EXPECT_FLOAT_EQ(*batch[0].score_signals.semantic_score, 0.6f);

    auto by_doc = store.GetChunksByDocId("d1");
    ASSERT_EQ(by_doc.size(), 2u);
    ASSERT_TRUE(by_doc[0].score_signals.enriched_score.has_value());
    ASSERT_TRUE(by_doc[1].score_signals.semantic_score.has_value());
}

TEST_F(SqliteChunkStoreTest, GetChunksByDocIdOrdersByChunkIndex) {
    InsertChild(301, "d1", 2, "c-c", "p1", "third");   // inserted out of order
    InsertChild(302, "d1", 0, "c-a", "p1", "first");
    InsertChild(303, "d1", 1, "c-b", "p1", "second");
    SqliteChunkStore store(db_);
    auto recs = store.GetChunksByDocId("d1");
    ASSERT_EQ(recs.size(), 3u);
    EXPECT_EQ(recs[0].content, "first");
    EXPECT_EQ(recs[1].content, "second");
    EXPECT_EQ(recs[2].content, "third");
}

TEST_F(SqliteChunkStoreTest, GetChunksByDocIdExcludesMetaAndOtherDocs) {
    InsertChild(401, "d1", 0, "c1", "p1", "d1 chunk");
    InsertMeta(402, "d1");                              // non-child META — excluded
    InsertChild(403, "d2", 0, "c2", "p2", "d2 chunk");  // other doc — excluded
    SqliteChunkStore store(db_);
    auto recs = store.GetChunksByDocId("d1");
    ASSERT_EQ(recs.size(), 1u);
    EXPECT_EQ(recs[0].child_id, "c1");
}

TEST_F(SqliteChunkStoreTest, GetChunksByDocIdEmptyWhenOnlyMeta) {
    InsertMeta(501, "d1");
    SqliteChunkStore store(db_);
    EXPECT_TRUE(store.GetChunksByDocId("d1").empty());
}

TEST_F(SqliteChunkStoreTest, NullDbHandleIsSafe) {
    SqliteChunkStore store(nullptr);
    EXPECT_FALSE(store.Get("c1").ok());
    std::vector<std::string> missing;
    EXPECT_TRUE(store.GetBatch({"c1", "c2"}, &missing).empty());
    EXPECT_EQ(missing.size(), 2u);   // a null handle routes every id to missing
    EXPECT_TRUE(store.GetChunksByDocId("d1").empty());
}

}  // namespace
}  // namespace cortrix::store
