#include <gtest/gtest.h>

#include <sqlite3.h>

#include <string>

#include "cortrix/scoring/scoring_schema_provider.h"

// Semantic score S8 coverage: ScoringSchemaProvider adds the per-Unit blocks.semantic_score column
// (ARCH §5.1.2) idempotently, inside the frozen ISchemaProvider contract. Mirrors the enricher
// provider's ADD-COLUMN-if-absent pattern (test_f03_schema_provider.cpp).
namespace cortrix::scoring {
namespace {

// Minimal stand-in for the F09-owned per-Unit blocks table (the real per-Unit framework
// creates the full table; we only need enough shape to ALTER + index).
constexpr const char* kBlocksSql = R"SQL(
CREATE TABLE blocks (
    block_id        INTEGER PRIMARY KEY AUTOINCREMENT,
    doc_id          INTEGER NOT NULL,
    chunk_index     INTEGER NOT NULL,
    block_type      INTEGER NOT NULL,
    processing_level INTEGER NOT NULL,
    data            BLOB NOT NULL
);
)SQL";

class ScoringSchemaTest : public ::testing::Test {
protected:
    void SetUp() override { ASSERT_EQ(sqlite3_open(":memory:", &db_), SQLITE_OK); }
    void TearDown() override { sqlite3_close(db_); }

    int Exec(const std::string& sql) {
        return sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, nullptr);
    }
    void CreateBlocks() { ASSERT_EQ(Exec(kBlocksSql), SQLITE_OK); }

    bool ColumnExists(const char* column) {
        std::string sql =
            std::string("SELECT 1 FROM pragma_table_info('blocks') WHERE name='") + column + "';";
        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
        bool found = (sqlite3_step(stmt) == SQLITE_ROW);
        sqlite3_finalize(stmt);
        return found;
    }
    bool IndexExists(const std::string& name) {
        std::string sql =
            "SELECT 1 FROM sqlite_master WHERE type='index' AND name='" + name + "';";
        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
        bool found = (sqlite3_step(stmt) == SQLITE_ROW);
        sqlite3_finalize(stmt);
        return found;
    }

    sqlite3* db_ = nullptr;
};

TEST_F(ScoringSchemaTest, FeatureIdentity) {
    ScoringSchemaProvider p;
    EXPECT_EQ(p.FeatureName(), "F07");
    EXPECT_EQ(p.CurrentVersion(), kScoringSchemaVersion);
    EXPECT_EQ(kScoringSchemaVersion, 1);
}

TEST_F(ScoringSchemaTest, MigrateAddsSemanticScoreColumnAndIndex) {
    CreateBlocks();
    ScoringSchemaProvider p;
    ASSERT_TRUE(p.Migrate(db_, 0, 1).ok());
    EXPECT_TRUE(ColumnExists("semantic_score"));
    EXPECT_TRUE(IndexExists("idx_blocks_semantic_score"));
    // The new column accepts a REAL score and defaults to NULL.
    EXPECT_EQ(Exec("INSERT INTO blocks(doc_id, chunk_index, block_type, processing_level, data) "
                   "VALUES (1, 0, 1, 2, x'00');"), SQLITE_OK);
    EXPECT_EQ(Exec("UPDATE blocks SET semantic_score=0.6 WHERE block_id=1;"), SQLITE_OK);
}

TEST_F(ScoringSchemaTest, MigrateIsIdempotent) {
    CreateBlocks();
    ScoringSchemaProvider p;
    ASSERT_TRUE(p.Migrate(db_, 0, 1).ok());
    // Re-run: column already exists → no-op (ADD COLUMN would otherwise error).
    EXPECT_TRUE(p.Migrate(db_, 0, 1).ok());
    EXPECT_TRUE(p.Migrate(db_, 1, 1).ok());  // already-current no-op
}

// No blocks table (isolated unit context) → no-op, doesn't block the migrator batch.
TEST_F(ScoringSchemaTest, NoBlocksTableIsNoOp) {
    ScoringSchemaProvider p;
    EXPECT_TRUE(p.Migrate(db_, 0, 1).ok());
    EXPECT_FALSE(ColumnExists("semantic_score"));  // nothing created
}

TEST_F(ScoringSchemaTest, UnsupportedVersionStepRejected) {
    ScoringSchemaProvider p;
    Status s = p.Migrate(db_, 1, 2);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kInvalidArgument);
    EXPECT_NE(s.message().find("CX_ERR_SCHEMA_VERSION_MISMATCH"), std::string::npos);
}

// Semantic score + enricher co-existence: both ALTER blocks for their own column without clobbering.
TEST_F(ScoringSchemaTest, CoexistsWithEnrichedScoreColumn) {
    CreateBlocks();
    // Simulate enricher having already added enriched_score.
    ASSERT_EQ(Exec("ALTER TABLE blocks ADD COLUMN enriched_score REAL DEFAULT NULL;"), SQLITE_OK);
    ScoringSchemaProvider p;
    ASSERT_TRUE(p.Migrate(db_, 0, 1).ok());
    EXPECT_TRUE(ColumnExists("enriched_score"));   // Enricher's column untouched
    EXPECT_TRUE(ColumnExists("semantic_score"));   // Semantic score's column added
}

}  // namespace
}  // namespace cortrix::scoring
