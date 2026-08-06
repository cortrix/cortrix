#include "cortrix/spc_enricher/enricher_schema_provider.h"

#include <gtest/gtest.h>

#include <string>

#include <sqlite3.h>

#include "cortrix/catalog/schema_provider.h"

// design (v1.0.2 Major-2): EnricherSchemaProvider extends the per-Unit blocks
// table (+3 cols) + adds entities + FTS5, registered via the catalog SchemaMigrator.
namespace cortrix::spc {
namespace {

// Minimal stand-in for block header's per-Unit `blocks` table (only the shape enricher's
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

TEST(EnricherSchemaProviderTest, IdentityAndVersion) {
    EnricherSchemaProvider p;
    EXPECT_EQ(p.FeatureName(), "enricher");
    EXPECT_EQ(p.CurrentVersion(), 2);
}

TEST(EnricherSchemaProviderTest, AddsThreeBlockColumnsAndEntities) {
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    CreateBlocksTable(db);
    EXPECT_FALSE(HasColumn(db, "blocks", "enriched_score"));

    EnricherSchemaProvider p;
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

TEST(EnricherSchemaProviderTest, MigrationIdempotent) {
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    CreateBlocksTable(db);
    EnricherSchemaProvider p;
    ASSERT_TRUE(p.Migrate(db, 0, 1).ok());
    // Re-run + already-current must not error or duplicate.
    EXPECT_TRUE(p.Migrate(db, 0, 1).ok());
    EXPECT_TRUE(p.Migrate(db, 1, 1).ok());
    EXPECT_TRUE(HasColumn(db, "blocks", "enriched_score"));
    EXPECT_TRUE(HasTable(db, "entities"));
    sqlite3_close(db);
}

TEST(EnricherSchemaProviderTest, NoBlocksTableNoOp) {
    // Isolated test without the per-Unit framework → migrate is a no-op, not error.
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    EnricherSchemaProvider p;
    EXPECT_TRUE(p.Migrate(db, 0, 1).ok());
    EXPECT_FALSE(HasTable(db, "entities"));  // nothing created without blocks
    sqlite3_close(db);
}

TEST(EnricherSchemaProviderTest, UnexpectedVersionStepIsError) {
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    EnricherSchemaProvider p;
    Status st = p.Migrate(db, 2, 3);  // beyond CurrentVersion() (2) — no v3 yet
    EXPECT_FALSE(st.ok());
    EXPECT_NE(st.message().find("CX_ERR_SCHEMA_VERSION_MISMATCH"), std::string::npos);
    sqlite3_close(db);
}

// A null db is rejected up front (the !db guard in Migrate), distinct from the
// no-blocks-table no-op. The init check (from_ver/to_ver) passes first, so this
// pins the null-db arm specifically.
TEST(EnricherSchemaProviderTest, NullDbInvalidArgument) {
    EnricherSchemaProvider p;
    Status st = p.Migrate(nullptr, 0, 1);
    EXPECT_FALSE(st.ok());
    EXPECT_NE(st.message().find("null db"), std::string::npos);
}

// A no-op migration step where from_ver == to_ver but != the init (0->1) case:
// the `!init && from_ver != to_ver` guard is false (from==to), so it proceeds to
// the db/blocks checks rather than erroring. With no blocks table → Ok no-op.
TEST(EnricherSchemaProviderTest, SameVersionNonInitProceeds) {
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    EnricherSchemaProvider p;
    // 2->2: not the init pair, but from==to → not a mismatch error; no blocks → Ok.
    EXPECT_TRUE(p.Migrate(db, 2, 2).ok());
    sqlite3_close(db);
}

// Re-running migration when the +3 columns ALREADY exist exercises the
// AddBlocksColumnIfAbsent "ColumnExists → return Ok" short-circuit for each
// column (no duplicate ADD COLUMN). Pre-create the columns by hand so the very
// first Migrate hits the already-present branch.
TEST(EnricherSchemaProviderTest, ColumnsAlreadyPresentShortCircuits) {
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(db,
        "CREATE TABLE blocks (block_id INTEGER PRIMARY KEY AUTOINCREMENT, "
        "doc_id TEXT, enriched_score REAL, enriched_at INTEGER, "
        "enricher_metadata TEXT)",
        nullptr, nullptr, nullptr), SQLITE_OK);
    EnricherSchemaProvider p;
    // Columns already there → each AddBlocksColumnIfAbsent short-circuits to Ok;
    // entities + indexes are still created.
    ASSERT_TRUE(p.Migrate(db, 0, 1).ok());
    EXPECT_TRUE(HasColumn(db, "blocks", "enriched_score"));
    EXPECT_TRUE(HasTable(db, "entities"));
    EXPECT_TRUE(HasTable(db, "entities_fts"));
    sqlite3_close(db);
}

// Runs through the real SchemaMigrator per-Unit path (the catalog integration point).
TEST(EnricherSchemaProviderTest, RegistersAndMigratesViaMigratorUnit) {
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    CreateBlocksTable(db);
    EnricherSchemaProvider p;
    cortrix::catalog::SchemaMigrator m;
    m.Register(&p);
    Status st = m.MigrateUnit(db, "unit-1");
    ASSERT_TRUE(st.ok()) << st.message();
    EXPECT_EQ(m.CurrentVersion(db, "enricher"), 2);
    EXPECT_TRUE(HasColumn(db, "blocks", "enriched_score"));
    EXPECT_TRUE(HasTable(db, "entities"));
    sqlite3_close(db);
}

// R9 Tier C regression: the entities.block_id FK must be ON DELETE CASCADE so GC
// hard-delete / purge (DELETE FROM blocks) reclaims blocks instead of failing with
// "FOREIGN KEY constraint failed". Pins the live finding that unit-level GC tests
// missed — their store had no entity rows pinning the blocks.
TEST(EnricherSchemaProviderTest, EntitiesFkCascadeAllowsBlockDelete) {
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(db, "PRAGMA foreign_keys=ON", nullptr, nullptr, nullptr), SQLITE_OK);
    CreateBlocksTable(db);
    EnricherSchemaProvider p;
    ASSERT_TRUE(p.Migrate(db, 0, 1).ok());
    ASSERT_EQ(sqlite3_exec(db,
        "INSERT INTO blocks (block_id, doc_id, chunk_index, content_text) VALUES (1,'d1',0,'x');"
        "INSERT INTO entities (block_id, text, type) VALUES (1,'Acme','ORG');",
        nullptr, nullptr, nullptr), SQLITE_OK);
    // Deleting the block must succeed and cascade-remove the entity (not FK-fail).
    char* err = nullptr;
    EXPECT_EQ(sqlite3_exec(db, "DELETE FROM blocks WHERE block_id=1", nullptr, nullptr, &err),
              SQLITE_OK) << (err ? err : "");
    sqlite3_free(err);
    sqlite3_stmt* st = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM entities", -1, &st, nullptr), SQLITE_OK);
    ASSERT_EQ(sqlite3_step(st), SQLITE_ROW);
    EXPECT_EQ(sqlite3_column_int(st, 0), 0);  // cascade removed the orphan entity
    sqlite3_finalize(st);
    sqlite3_close(db);
}

// The in-place rebuild of a pre-CASCADE entities table preserves rows and yields a
// cascading FK. Simulates an existing unit created before the R9 Tier C fix.
TEST(EnricherSchemaProviderTest, RebuildsPreCascadeEntitiesTable) {
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(db, "PRAGMA foreign_keys=ON", nullptr, nullptr, nullptr), SQLITE_OK);
    CreateBlocksTable(db);
    // Old-shape entities table (no ON DELETE CASCADE) + a block and a referencing entity.
    ASSERT_EQ(sqlite3_exec(db,
        "CREATE TABLE entities (entity_id INTEGER PRIMARY KEY, "
        "  block_id INTEGER NOT NULL REFERENCES blocks(block_id), "
        "  text TEXT NOT NULL, type TEXT NOT NULL, start_offset INTEGER, end_offset INTEGER);"
        "INSERT INTO blocks (block_id, doc_id, chunk_index, content_text) VALUES (1,'d1',0,'x');"
        "INSERT INTO entities (entity_id, block_id, text, type) VALUES (7,1,'Acme','ORG');",
        nullptr, nullptr, nullptr), SQLITE_OK);
    // Pre-state: deleting the block FK-fails (the bug being fixed).
    EXPECT_NE(sqlite3_exec(db, "DELETE FROM blocks WHERE block_id=1", nullptr, nullptr, nullptr),
              SQLITE_OK);

    EnricherSchemaProvider p;
    ASSERT_TRUE(p.Migrate(db, 0, 1).ok());  // rebuilds entities with CASCADE

    // Row preserved through the rebuild.
    sqlite3_stmt* st = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(db, "SELECT block_id FROM entities WHERE entity_id=7",
                                 -1, &st, nullptr), SQLITE_OK);
    ASSERT_EQ(sqlite3_step(st), SQLITE_ROW);
    EXPECT_EQ(sqlite3_column_int(st, 0), 1);
    sqlite3_finalize(st);

    // Now the block delete cascades cleanly.
    char* err = nullptr;
    EXPECT_EQ(sqlite3_exec(db, "DELETE FROM blocks WHERE block_id=1", nullptr, nullptr, &err),
              SQLITE_OK) << (err ? err : "");
    sqlite3_free(err);
    sqlite3_close(db);
}

}  // namespace
}  // namespace cortrix::spc
