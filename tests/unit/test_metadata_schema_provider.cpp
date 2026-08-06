#include <gtest/gtest.h>

#include <sqlite3.h>

#include <string>

#include "cortrix/metadata/metadata_schema_provider.h"

// META block S1.3 coverage: MetadataSchemaProvider creates metadata_blocks + idx_metablocks_ns
// (detailed design, locked separate table) idempotently, inside the frozen ISchemaProvider contract.
// Mirrors tests/unit/test_import_schema_provider.cpp.
namespace cortrix::metadata {
namespace {

class MetadataSchemaTest : public ::testing::Test {
protected:
    void SetUp() override { ASSERT_EQ(sqlite3_open(":memory:", &db_), SQLITE_OK); }
    void TearDown() override { sqlite3_close(db_); }

    bool TableExists(const std::string& name) {
        std::string sql =
            "SELECT 1 FROM sqlite_master WHERE type='table' AND name='" + name + "';";
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

    int Exec(const std::string& sql) {
        return sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, nullptr);
    }

    sqlite3* db_ = nullptr;
};

TEST_F(MetadataSchemaTest, FeatureIdentity) {
    MetadataSchemaProvider p;
    EXPECT_EQ(p.FeatureName(), "metadata_block");
    EXPECT_EQ(p.CurrentVersion(), kMetadataSchemaVersion);
    EXPECT_EQ(kMetadataSchemaVersion, 1);
}

TEST_F(MetadataSchemaTest, MigrateCreatesTableAndIndex) {
    MetadataSchemaProvider p;
    Status s = p.Migrate(db_, 0, kMetadataSchemaVersion);
    ASSERT_TRUE(s.ok()) << s.message();
    EXPECT_TRUE(TableExists("metadata_blocks"));
    EXPECT_TRUE(IndexExists("idx_metablocks_ns"));
}

TEST_F(MetadataSchemaTest, MigrateIsIdempotentOnRerun) {
    MetadataSchemaProvider p;
    ASSERT_TRUE(p.Migrate(db_, 0, 1).ok());
    // A second 0→1 (CREATE ... IF NOT EXISTS) is a no-op, not an error.
    EXPECT_TRUE(p.Migrate(db_, 0, 1).ok());
    // An already-current 1→1 call is a defensive no-op.
    EXPECT_TRUE(p.Migrate(db_, 1, 1).ok());
}

TEST_F(MetadataSchemaTest, UnsupportedVersionStepRejected) {
    MetadataSchemaProvider p;
    Status s = p.Migrate(db_, 1, 2);  // Phase 2 not implemented yet
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kInvalidArgument);
    EXPECT_NE(s.message().find("CX_ERR_SCHEMA_VERSION_MISMATCH"), std::string::npos);
}

// The created table accepts a row shaped like a real metadata block, and doc_id is
// UNIQUE (1 doc = 1 metadata block — detailed design).
TEST_F(MetadataSchemaTest, TableShapeAcceptsRowAndEnforcesDocIdUnique) {
    MetadataSchemaProvider p;
    ASSERT_TRUE(p.Migrate(db_, 0, 1).ok());

    EXPECT_EQ(Exec("INSERT INTO metadata_blocks(block_id, doc_id, namespace_id, block_text, "
                   "metadata_json) VALUES ('01B', 'doc1', 'ns', 'text', '{}');"),
              SQLITE_OK);
    // created_at defaults to now (Unix ms) — non-null.
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_, "SELECT created_at FROM metadata_blocks WHERE block_id='01B';",
                       -1, &stmt, nullptr);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    EXPECT_GT(sqlite3_column_int64(stmt, 0), 0);
    sqlite3_finalize(stmt);

    // Second block for the same doc_id violates UNIQUE.
    EXPECT_NE(Exec("INSERT INTO metadata_blocks(block_id, doc_id, namespace_id, block_text, "
                   "metadata_json) VALUES ('01C', 'doc1', 'ns', 'text2', '{}');"),
              SQLITE_OK);
}

}  // namespace
}  // namespace cortrix::metadata
