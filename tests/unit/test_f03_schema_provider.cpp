#include "cortrix/spc_enricher/f03_schema_provider.h"

#include <gtest/gtest.h>

#include <string>

#include <sqlite3.h>

#include "cortrix/catalog/schema_provider.h"

// design §3.1 (v1.0.2 Major-2): F03SchemaProvider extends the per-Unit blocks
// table (+3 cols) + adds entities + FTS5, registered via the F12 SchemaMigrator.
namespace cortrix::spc {
namespace {

// Minimal stand-in for F09's per-Unit `blocks` table (only the shape F03's
// migration touches — standalone, no full per-Unit framework).
void CreateBlocksTable(sqlite3* db) {
    ASSERT_EQ(sqlite3_exec(db,
        "CREATE TABLE blocks (block_id INTEGER PRIMARY KEY AUTOINCREMENT, "
        "doc_id TEXT, chunk_index INTEGER, content_text TEXT)",
        nullptr, nullptr, nullptr), SQLITE_OK);
}

bool HasColumn(sqlite3* db, const char* table, const char* column) {
    sqlite3_stmt* stmt = nullptr;
    EXPECT_EQ(sqlite3_prepare_v2(
        db, "SELECT 1 FROM pragma_table_info(?1) WHERE name=?2", -1, &stmt, nullptr),
        SQLITE_OK);
    sqlite3_bind_text(stmt, 1, table, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, column, -1, SQLITE_STATIC);
    const bool found = (sqlite3_step(stmt) == SQLITE_ROW);
    sqlite3_finalize(stmt);
    return found;
}

bool HasTable(sqlite3* db, const char* table) {
    sqlite3_stmt* stmt = nullptr;
    EXPECT_EQ(sqlite3_prepare_v2(
        db, "SELECT 1 FROM sqlite_master WHERE name=?1", -1, &stmt, nullptr), SQLITE_OK);
    sqlite3_bind_text(stmt, 1, table, -1, SQLITE_STATIC);
    const bool found = (sqlite3_step(stmt) == SQLITE_ROW);
    sqlite3_finalize(stmt);
    return found;
}

bool HasIndex(sqlite3* db, const char* index) {
    sqlite3_stmt* stmt = nullptr;
    EXPECT_EQ(sqlite3_prepare_v2(
        db, "SELECT 1 FROM sqlite_master WHERE type='index' AND name=?1", -1, &stmt, nullptr),
        SQLITE_OK);
    sqlite3_bind_text(stmt, 1, index, -1, SQLITE_STATIC);
    const bool found = (sqlite3_step(stmt) == SQLITE_ROW);
    sqlite3_finalize(stmt);
    return found;
}

TEST(F03SchemaProviderTest, IdentityAndVersion) {
    F03SchemaProvider p;
    EXPECT_EQ(p.FeatureName(), "F03");
    EXPECT_EQ(p.CurrentVersion(), 1);
}

TEST(F03SchemaProviderTest, AddsThreeBlockColumnsAndEntities) {
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    CreateBlocksTable(db);
    EXPECT_FALSE(HasColumn(db, "blocks", "enriched_score"));

    F03SchemaProvider p;
    ASSERT_TRUE(p.Migrate(db, 0, 1).ok());

    EXPECT_TRUE(HasColumn(db, "blocks", "enriched_score"));
    EXPECT_TRUE(HasColumn(db, "blocks", "enriched_at"));
    EXPECT_TRUE(HasColumn(db, "blocks", "enricher_metadata"));
    EXPECT_TRUE(HasIndex(db, "idx_blocks_enriched_score"));
    EXPECT_TRUE(HasTable(db, "entities"));
    EXPECT_TRUE(HasTable(db, "entities_fts"));
    EXPECT_TRUE(HasIndex(db, "idx_entities_block"));
    EXPECT_TRUE(HasIndex(db, "idx_entities_type"));
    sqlite3_close(db);
}

TEST(F03SchemaProviderTest, MigrationIdempotent) {
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    CreateBlocksTable(db);
    F03SchemaProvider p;
    ASSERT_TRUE(p.Migrate(db, 0, 1).ok());
    // Re-run + already-current must not error or duplicate.
    EXPECT_TRUE(p.Migrate(db, 0, 1).ok());
    EXPECT_TRUE(p.Migrate(db, 1, 1).ok());
    EXPECT_TRUE(HasColumn(db, "blocks", "enriched_score"));
    EXPECT_TRUE(HasTable(db, "entities"));
    sqlite3_close(db);
}

TEST(F03SchemaProviderTest, NoBlocksTableNoOp) {
    // Isolated test without the per-Unit framework → migrate is a no-op, not error.
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    F03SchemaProvider p;
    EXPECT_TRUE(p.Migrate(db, 0, 1).ok());
    EXPECT_FALSE(HasTable(db, "entities"));  // nothing created without blocks
    sqlite3_close(db);
}

TEST(F03SchemaProviderTest, UnexpectedVersionStepIsError) {
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    F03SchemaProvider p;
    Status st = p.Migrate(db, 1, 2);  // no Phase 2 bump yet
    EXPECT_FALSE(st.ok());
    EXPECT_NE(st.message().find("CX_ERR_SCHEMA_VERSION_MISMATCH"), std::string::npos);
    sqlite3_close(db);
}

// A null db is rejected up front (the !db guard in Migrate), distinct from the
// no-blocks-table no-op. The init check (from_ver/to_ver) passes first, so this
// pins the null-db arm specifically.
TEST(F03SchemaProviderTest, NullDbInvalidArgument) {
    F03SchemaProvider p;
    Status st = p.Migrate(nullptr, 0, 1);
    EXPECT_FALSE(st.ok());
    EXPECT_NE(st.message().find("null db"), std::string::npos);
}

// A no-op migration step where from_ver == to_ver but != the init (0->1) case:
// the `!init && from_ver != to_ver` guard is false (from==to), so it proceeds to
// the db/blocks checks rather than erroring. With no blocks table → Ok no-op.
TEST(F03SchemaProviderTest, SameVersionNonInitProceeds) {
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    F03SchemaProvider p;
    // 2->2: not the init pair, but from==to → not a mismatch error; no blocks → Ok.
    EXPECT_TRUE(p.Migrate(db, 2, 2).ok());
    sqlite3_close(db);
}

// Re-running migration when the +3 columns ALREADY exist exercises the
// AddBlocksColumnIfAbsent "ColumnExists → return Ok" short-circuit for each
// column (no duplicate ADD COLUMN). Pre-create the columns by hand so the very
// first Migrate hits the already-present branch.
TEST(F03SchemaProviderTest, ColumnsAlreadyPresentShortCircuits) {
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(db,
        "CREATE TABLE blocks (block_id INTEGER PRIMARY KEY AUTOINCREMENT, "
        "doc_id TEXT, enriched_score REAL, enriched_at INTEGER, "
        "enricher_metadata TEXT)",
        nullptr, nullptr, nullptr), SQLITE_OK);
    F03SchemaProvider p;
    // Columns already there → each AddBlocksColumnIfAbsent short-circuits to Ok;
    // entities + indexes are still created.
    ASSERT_TRUE(p.Migrate(db, 0, 1).ok());
    EXPECT_TRUE(HasColumn(db, "blocks", "enriched_score"));
    EXPECT_TRUE(HasTable(db, "entities"));
    EXPECT_TRUE(HasTable(db, "entities_fts"));
    sqlite3_close(db);
}

// Runs through the real SchemaMigrator per-Unit path (the F12 integration point).
TEST(F03SchemaProviderTest, RegistersAndMigratesViaMigratorUnit) {
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    CreateBlocksTable(db);
    F03SchemaProvider p;
    cortrix::catalog::SchemaMigrator m;
    m.Register(&p);
    Status st = m.MigrateUnit(db, "unit-1");
    ASSERT_TRUE(st.ok()) << st.message();
    EXPECT_EQ(m.CurrentVersion(db, "F03"), 1);
    EXPECT_TRUE(HasColumn(db, "blocks", "enriched_score"));
    EXPECT_TRUE(HasTable(db, "entities"));
    sqlite3_close(db);
}

}  // namespace
}  // namespace cortrix::spc
