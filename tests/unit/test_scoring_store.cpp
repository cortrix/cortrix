#include "cortrix/scoring/scoring_store.h"

#include <gtest/gtest.h>

#include <sqlite3.h>

#include "cortrix/scoring/scoring_schema_provider.h"

namespace cortrix::scoring {
namespace {

// Per-Unit DB with a blocks table + the semantic score schema (semantic_score column) applied,
// plus one block row to attach a score to. Mirrors test_enricher_store's fixture.
class ScoringStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_EQ(sqlite3_open(":memory:", &db_), SQLITE_OK);
        ASSERT_EQ(sqlite3_exec(db_,
            "CREATE TABLE blocks (block_id INTEGER PRIMARY KEY, doc_id TEXT, "
            "chunk_index INTEGER, content_text TEXT)",
            nullptr, nullptr, nullptr), SQLITE_OK);
        ScoringSchemaProvider p;
        ASSERT_TRUE(p.Migrate(db_, 0, 1).ok());
        ASSERT_EQ(sqlite3_exec(db_,
            "INSERT INTO blocks(block_id, doc_id, chunk_index, content_text) "
            "VALUES(1,'d1',0,'hello')",
            nullptr, nullptr, nullptr), SQLITE_OK);
    }
    void TearDown() override { sqlite3_close(db_); }

    // Reads semantic_score; returns -1.0 when the column is NULL (never written).
    double Score(uint64_t block_id) {
        sqlite3_stmt* st = nullptr;
        EXPECT_EQ(sqlite3_prepare_v2(
            db_, "SELECT semantic_score FROM blocks WHERE block_id=?1", -1, &st, nullptr),
            SQLITE_OK);
        sqlite3_bind_int64(st, 1, block_id);
        double v = -1.0;
        if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_type(st, 0) != SQLITE_NULL) {
            v = sqlite3_column_double(st, 0);
        }
        sqlite3_finalize(st);
        return v;
    }

    sqlite3* db_ = nullptr;
};

TEST_F(ScoringStoreTest, WritesSemanticScore) {
    ASSERT_TRUE(WriteScore(db_, 1, 0.6f).ok());
    EXPECT_NEAR(Score(1), 0.6, 1e-6);
}

TEST_F(ScoringStoreTest, OverwritesExistingScore) {
    ASSERT_TRUE(WriteScore(db_, 1, 0.4f).ok());
    ASSERT_TRUE(WriteScore(db_, 1, 1.0f).ok());
    EXPECT_NEAR(Score(1), 1.0, 1e-6);
}

TEST_F(ScoringStoreTest, AnomalySentinelZeroIsPersisted) {
    // D6: an anomalous block's 0.0 is a real value, distinct from NULL ("never scored").
    ASSERT_TRUE(WriteScore(db_, 1, 0.0f).ok());
    EXPECT_DOUBLE_EQ(Score(1), 0.0);
}

TEST_F(ScoringStoreTest, UnscoredBlockReadsNull) {
    EXPECT_DOUBLE_EQ(Score(1), -1.0);  // column default NULL before any WriteScore
}

TEST_F(ScoringStoreTest, NullDbRejected) {
    EXPECT_FALSE(WriteScore(nullptr, 1, 0.6f).ok());
}

}  // namespace
}  // namespace cortrix::scoring
