#include <gtest/gtest.h>

#include <sqlite3.h>

#include <cstdint>
#include <cstdio>
#include <string>

#include "cortrix/catalog/schema_provider.h"
#include "cortrix/store/block_framework_schema_provider.h"

// BlockFrameworkSchemaProvider now OWNS the per-Unit framework schema (documents /
// blocks / blocks_fts + indices + triggers) from kPerUnitFrameworkDdl, run via
// SchemaMigrator::MigrateUnit at namespace pool load. These tests are the exact-preservation
// guard for the schema moved out of CortrixStoreSqlite::CreateTables (QA M3 five-piece set:
// object presence + FTS5 roundtrip + 3 triggers + block_id rowid-alias). PRAGMAs
// are applied by the conn (SqliteConn/ExecutePragma), not this DDL, so unchanged.
namespace cortrix::store {
namespace {

bool ObjectExists(sqlite3* db, const std::string& name) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT 1 FROM sqlite_master WHERE name = ?", -1, &stmt,
                           nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_TRANSIENT);
    bool exists = (sqlite3_step(stmt) == SQLITE_ROW);
    sqlite3_finalize(stmt);
    return exists;
}

int ScalarInt(sqlite3* db, const std::string& sql) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) return -1;
    int n = (sqlite3_step(stmt) == SQLITE_ROW) ? sqlite3_column_int(stmt, 0) : -1;
    sqlite3_finalize(stmt);
    return n;
}

bool Exec(sqlite3* db, const std::string& sql) {
    return sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK;
}

TEST(BlockFrameworkSchemaProviderTest, IdentityAndVersion) {
    BlockFrameworkSchemaProvider p;
    EXPECT_EQ(p.FeatureName(), "block_framework");
    EXPECT_EQ(p.CurrentVersion(), 1);
}

// five-piece set #1: all framework objects present (tables + indices + triggers + FTS).
TEST(BlockFrameworkSchemaProviderTest, MigrateBuildsFrameworkSchema) {
    BlockFrameworkSchemaProvider p;
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    ASSERT_TRUE(p.Migrate(db, 0, 1).ok());

    EXPECT_TRUE(ObjectExists(db, "documents"));
    EXPECT_TRUE(ObjectExists(db, "blocks"));
    EXPECT_TRUE(ObjectExists(db, "blocks_fts"));
    EXPECT_TRUE(ObjectExists(db, "idx_doc_source"));
    EXPECT_TRUE(ObjectExists(db, "idx_block_doc"));
    EXPECT_TRUE(ObjectExists(db, "idx_block_hnsw"));
    EXPECT_TRUE(ObjectExists(db, "blocks_ai"));
    EXPECT_TRUE(ObjectExists(db, "blocks_ad"));
    EXPECT_TRUE(ObjectExists(db, "blocks_au"));

    EXPECT_TRUE(p.Migrate(db, 1, 1).ok());  // already current = no-op success
    sqlite3_close(db);
}

// five-piece set #2/#3: FTS5 external-content roundtrip exercises all 3 triggers.
TEST(BlockFrameworkSchemaProviderTest, FtsRoundtripAndTriggers) {
    BlockFrameworkSchemaProvider p;
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    ASSERT_TRUE(p.Migrate(db, 0, 1).ok());

    ASSERT_TRUE(Exec(db, "INSERT INTO documents(doc_id, source_type, source_path) "
                         "VALUES('doc1','file','/p')"));
    // blocks_ai (INSERT) → FTS row created.
    ASSERT_TRUE(Exec(db, "INSERT INTO blocks(block_id, doc_id, chunk_index, block_type, "
                         "processing_level, data, content_text) "
                         "VALUES(1,'doc1',0,0,3,x'00','alpha bravo')"));
    EXPECT_EQ(ScalarInt(db, "SELECT count(*) FROM blocks_fts WHERE blocks_fts MATCH 'bravo'"), 1);

    // blocks_au (UPDATE) → FTS reflects new text, drops old.
    ASSERT_TRUE(Exec(db, "UPDATE blocks SET content_text='charlie delta' WHERE block_id=1"));
    EXPECT_EQ(ScalarInt(db, "SELECT count(*) FROM blocks_fts WHERE blocks_fts MATCH 'bravo'"), 0);
    EXPECT_EQ(ScalarInt(db, "SELECT count(*) FROM blocks_fts WHERE blocks_fts MATCH 'delta'"), 1);

    // blocks_ad (DELETE) → FTS row removed.
    ASSERT_TRUE(Exec(db, "DELETE FROM blocks WHERE block_id=1"));
    EXPECT_EQ(ScalarInt(db, "SELECT count(*) FROM blocks_fts WHERE blocks_fts MATCH 'delta'"), 0);
    sqlite3_close(db);
}

// five-piece set #4: block_id is an INTEGER PRIMARY KEY rowid alias (NOT autoincrement).
TEST(BlockFrameworkSchemaProviderTest, BlockIdIsRowidAliasNotAutoincrement) {
    BlockFrameworkSchemaProvider p;
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    ASSERT_TRUE(p.Migrate(db, 0, 1).ok());
    ASSERT_TRUE(Exec(db, "INSERT INTO documents(doc_id, source_type, source_path) "
                         "VALUES('d','f','/p')"));
    // App-provided block_id is honored verbatim (rowid alias semantics).
    ASSERT_TRUE(Exec(db, "INSERT INTO blocks(block_id, doc_id, chunk_index, block_type, "
                         "processing_level, data) VALUES(424242,'d',0,0,3,x'00')"));
    EXPECT_EQ(ScalarInt(db, "SELECT block_id FROM blocks"), 424242);
    EXPECT_EQ(ScalarInt(db, "SELECT rowid FROM blocks"), 424242);  // block_id == rowid
    EXPECT_FALSE(ObjectExists(db, "sqlite_sequence"));  // present iff AUTOINCREMENT used
    sqlite3_close(db);
}

TEST(BlockFrameworkSchemaProviderTest, UnexpectedVersionStepIsError) {
    BlockFrameworkSchemaProvider p;
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    Status st = p.Migrate(db, 1, 2);  // Phase 2 not implemented yet
    EXPECT_FALSE(st.ok());
    EXPECT_NE(st.message().find("CX_ERR_SCHEMA_VERSION_MISMATCH"), std::string::npos);
    sqlite3_close(db);
}

// --- branch-coverage supplement (DDL exec failure path) -------------------

TEST(BlockFrameworkSchemaProviderTest, FrameworkDdlExecFailureReported) {
    // A read-only connection makes the framework DDL's first CREATE statement fail,
    // so sqlite3_exec != SQLITE_OK and Migrate returns CX_ERR_SCHEMA_MIGRATION_FAILED
    // (the DDL exec error branch). Materialize an on-disk DB, then reopen read-only.
    const std::string path =
        std::string(::testing::TempDir()) + "f09_ro_" +
        std::to_string(reinterpret_cast<uintptr_t>(&path)) + ".db";
    sqlite3* seed = nullptr;
    ASSERT_EQ(sqlite3_open(path.c_str(), &seed), SQLITE_OK);
    ASSERT_EQ(sqlite3_close(seed), SQLITE_OK);

    sqlite3* ro = nullptr;
    ASSERT_EQ(sqlite3_open_v2(path.c_str(), &ro, SQLITE_OPEN_READONLY, nullptr), SQLITE_OK);
    BlockFrameworkSchemaProvider p;
    Status st = p.Migrate(ro, 0, 1);
    EXPECT_FALSE(st.ok());
    EXPECT_NE(st.message().find("CX_ERR_SCHEMA_MIGRATION_FAILED"), std::string::npos);
    sqlite3_close(ro);
    std::remove(path.c_str());
}

TEST(BlockFrameworkSchemaProviderTest, FromZeroToUnsupportedTargetIsError) {
    // from_ver==0 but to_ver!=1 (here 0→2): the init guard's second sub-condition
    // is false, so it falls through to the version-mismatch return — covering the
    // other arm of the `from_ver==0 && to_ver==1` compound condition.
    BlockFrameworkSchemaProvider p;
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    Status st = p.Migrate(db, 0, 2);
    EXPECT_FALSE(st.ok());
    EXPECT_NE(st.message().find("CX_ERR_SCHEMA_VERSION_MISMATCH"), std::string::npos);
    sqlite3_close(db);
}

// Production integration point: via SchemaMigrator::MigrateUnit (namespace pool load path).
TEST(BlockFrameworkSchemaProviderTest, MigratesViaMigrateUnit) {
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    BlockFrameworkSchemaProvider p;
    cortrix::catalog::SchemaMigrator m;
    m.Register(&p);
    Status st = m.MigrateUnit(db, "unit-test");
    ASSERT_TRUE(st.ok()) << st.message();
    EXPECT_EQ(m.CurrentVersion(db, "block_framework"), 1);
    EXPECT_TRUE(ObjectExists(db, "blocks"));  // framework built through the migrator
    sqlite3_close(db);
}

}  // namespace
}  // namespace cortrix::store
