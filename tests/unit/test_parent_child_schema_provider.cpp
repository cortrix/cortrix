#include "cortrix/store/parent_child_schema_provider.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <string>

#include <sqlite3.h>

#include "cortrix/catalog/schema_provider.h"

// Parent-child chunking (A unified-blocks): ParentChildSchemaProvider extends the
// per-Unit blocks table (+4 child cols) + 3 indexes (incl idx_blocks_meta_doc, B2)
// + creates the parents table, registered via the catalog SchemaMigrator.
namespace cortrix::store {
namespace {

// Minimal stand-in for block header's per-Unit `blocks` table (only the shape parent-child chunking's
// migration touches: block_id / doc_id / block_type — standalone, no full framework).
void CreateBlocksTable(sqlite3* db) {
    ASSERT_EQ(sqlite3_exec(db,
        "CREATE TABLE blocks (block_id INTEGER PRIMARY KEY AUTOINCREMENT, "
        "doc_id TEXT, chunk_index INTEGER, block_type INTEGER, content_text TEXT)",
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
        db, "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?1", -1, &stmt, nullptr),
        SQLITE_OK);
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

int ExecNoErr(sqlite3* db, const char* sql) {
    return sqlite3_exec(db, sql, nullptr, nullptr, nullptr);
}

TEST(ParentChildSchemaProviderTest, IdentityAndVersion) {
    ParentChildSchemaProvider p;
    EXPECT_EQ(p.FeatureName(), "parent_child");
    EXPECT_EQ(p.CurrentVersion(), 1);
}

TEST(ParentChildSchemaProviderTest, AddsChildColumnsIndexesAndParents) {
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    CreateBlocksTable(db);
    EXPECT_FALSE(HasColumn(db, "blocks", "child_id"));

    ParentChildSchemaProvider p;
    ASSERT_TRUE(p.Migrate(db, 0, 1).ok());

    // +4 child columns on blocks.
    EXPECT_TRUE(HasColumn(db, "blocks", "child_id"));
    EXPECT_TRUE(HasColumn(db, "blocks", "parent_id"));
    EXPECT_TRUE(HasColumn(db, "blocks", "token_count"));
    EXPECT_TRUE(HasColumn(db, "blocks", "parent_offset"));
    // 3 indexes.
    EXPECT_TRUE(HasIndex(db, "idx_blocks_child_id"));
    EXPECT_TRUE(HasIndex(db, "idx_blocks_parent"));
    EXPECT_TRUE(HasIndex(db, "idx_blocks_meta_doc"));
    // parents table + its indexes.
    EXPECT_TRUE(HasTable(db, "parents"));
    EXPECT_TRUE(HasIndex(db, "idx_parents_doc"));
    EXPECT_TRUE(HasIndex(db, "idx_parents_ns"));
    // No legacy children table created by the provider (A unified-blocks).
    EXPECT_FALSE(HasTable(db, "children"));
    sqlite3_close(db);
}

TEST(ParentChildSchemaProviderTest, ChildIdUniqueButNullsAllowed) {
    // idx_blocks_child_id is partial-unique (WHERE child_id IS NOT NULL): many rows
    // may have NULL child_id (non-child blocks); non-NULL child_id must be unique.
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    CreateBlocksTable(db);
    ParentChildSchemaProvider p;
    ASSERT_TRUE(p.Migrate(db, 0, 1).ok());

    // Two non-child rows (NULL child_id) — allowed.
    EXPECT_EQ(ExecNoErr(db, "INSERT INTO blocks(doc_id, chunk_index, block_type) "
                            "VALUES('d1', 0, 1)"), SQLITE_OK);
    EXPECT_EQ(ExecNoErr(db, "INSERT INTO blocks(doc_id, chunk_index, block_type) "
                            "VALUES('d1', 1, 1)"), SQLITE_OK);
    // A child row with a unique child_id — allowed.
    EXPECT_EQ(ExecNoErr(db, "INSERT INTO blocks(doc_id, chunk_index, block_type, child_id) "
                            "VALUES('d1', 2, 1, 'C1')"), SQLITE_OK);
    // A second row with the SAME child_id — rejected by the partial unique index.
    EXPECT_NE(ExecNoErr(db, "INSERT INTO blocks(doc_id, chunk_index, block_type, child_id) "
                            "VALUES('d1', 3, 1, 'C1')"), SQLITE_OK);
    sqlite3_close(db);
}

TEST(ParentChildSchemaProviderTest, MetaDocUniquePerDoc) {
    // idx_blocks_meta_doc (B2): at most one block_type=8 (META) row per doc_id;
    // non-META rows of the same doc are unconstrained.
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    CreateBlocksTable(db);
    ParentChildSchemaProvider p;
    ASSERT_TRUE(p.Migrate(db, 0, 1).ok());

    // First META block for d1 — allowed.
    EXPECT_EQ(ExecNoErr(db, "INSERT INTO blocks(doc_id, chunk_index, block_type) "
                            "VALUES('d1', 0, 8)"), SQLITE_OK);
    // Many non-META blocks for d1 — allowed.
    EXPECT_EQ(ExecNoErr(db, "INSERT INTO blocks(doc_id, chunk_index, block_type) "
                            "VALUES('d1', 1, 1)"), SQLITE_OK);
    EXPECT_EQ(ExecNoErr(db, "INSERT INTO blocks(doc_id, chunk_index, block_type) "
                            "VALUES('d1', 2, 1)"), SQLITE_OK);
    // A second META block for d1 — rejected (1 doc = 1 META).
    EXPECT_NE(ExecNoErr(db, "INSERT INTO blocks(doc_id, chunk_index, block_type) "
                            "VALUES('d1', 3, 8)"), SQLITE_OK);
    // A META block for a different doc — allowed.
    EXPECT_EQ(ExecNoErr(db, "INSERT INTO blocks(doc_id, chunk_index, block_type) "
                            "VALUES('d2', 0, 8)"), SQLITE_OK);
    sqlite3_close(db);
}

TEST(ParentChildSchemaProviderTest, MigrationIdempotent) {
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    CreateBlocksTable(db);
    ParentChildSchemaProvider p;
    ASSERT_TRUE(p.Migrate(db, 0, 1).ok());
    // Re-run + already-current must not error or duplicate.
    EXPECT_TRUE(p.Migrate(db, 0, 1).ok());
    EXPECT_TRUE(p.Migrate(db, 1, 1).ok());
    EXPECT_TRUE(HasColumn(db, "blocks", "child_id"));
    EXPECT_TRUE(HasTable(db, "parents"));
    sqlite3_close(db);
}

TEST(ParentChildSchemaProviderTest, NoBlocksTableStillCreatesParents) {
    // Without the per-Unit framework `blocks`, the blocks ALTERs no-op but `parents`
    // (independent of blocks) is still created, and migrate succeeds.
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    ParentChildSchemaProvider p;
    EXPECT_TRUE(p.Migrate(db, 0, 1).ok());
    EXPECT_TRUE(HasTable(db, "parents"));
    EXPECT_FALSE(HasIndex(db, "idx_blocks_child_id"));  // no blocks → no blocks index
    sqlite3_close(db);
}

TEST(ParentChildSchemaProviderTest, UnexpectedVersionStepIsError) {
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    ParentChildSchemaProvider p;
    Status st = p.Migrate(db, 1, 2);  // no Phase 2 bump yet
    EXPECT_FALSE(st.ok());
    EXPECT_NE(st.message().find("CX_ERR_SCHEMA_VERSION_MISMATCH"), std::string::npos);
    sqlite3_close(db);
}

// --- branch-coverage supplements (error / failure paths) ------------------

TEST(ParentChildSchemaProviderTest, NullDbRejected) {
    // db == nullptr → kInvalidArgument ("F34 migrate: null db") AFTER the
    // init/version gate but BEFORE any Exec.
    ParentChildSchemaProvider p;
    Status st = p.Migrate(nullptr, 0, 1);
    EXPECT_FALSE(st.ok());
    EXPECT_NE(st.message().find("null db"), std::string::npos);
}

TEST(ParentChildSchemaProviderTest, FromZeroToUnsupportedTargetIsError) {
    // from_ver==0 but to_ver!=1 (0→2): init is false and from_ver != to_ver, so
    // the version-mismatch return fires — the other arm of the init compound
    // condition than UnexpectedVersionStepIsError (which uses 1→2).
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    ParentChildSchemaProvider p;
    Status st = p.Migrate(db, 0, 2);
    EXPECT_FALSE(st.ok());
    EXPECT_NE(st.message().find("CX_ERR_SCHEMA_VERSION_MISMATCH"), std::string::npos);
    sqlite3_close(db);
}

TEST(ParentChildSchemaProviderTest, SameVersionNoOpSucceeds) {
    // init==false but from_ver==to_ver (2→2): the `!init && from_ver != to_ver`
    // guard is false, so it proceeds (no version error) — the version-equal arm.
    // With no blocks table it just (re)creates parents idempotently and returns Ok.
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    ParentChildSchemaProvider p;
    Status st = p.Migrate(db, 2, 2);
    EXPECT_TRUE(st.ok()) << st.message();
    EXPECT_TRUE(HasTable(db, "parents"));
    sqlite3_close(db);
}

TEST(ParentChildSchemaProviderTest, ReadOnlyDbFailsParentsExec) {
    // A read-only connection makes the very first DDL (CREATE parents) fail, so
    // Exec returns CX_ERR_SCHEMA_MIGRATION_FAILED (the Exec error branch + the
    // parents error-return).
    sqlite3* db = nullptr;
    // Materialize an on-disk DB first (read-only :memory: is not meaningful), then
    // reopen it read-only.
    const std::string path =
        (std::string(::testing::TempDir()) + "f34_ro_" +
         std::to_string(reinterpret_cast<uintptr_t>(&db)) + ".db");
    ASSERT_EQ(sqlite3_open(path.c_str(), &db), SQLITE_OK);
    ASSERT_EQ(sqlite3_close(db), SQLITE_OK);

    sqlite3* ro = nullptr;
    ASSERT_EQ(sqlite3_open_v2(path.c_str(), &ro, SQLITE_OPEN_READONLY, nullptr), SQLITE_OK);
    ParentChildSchemaProvider p;
    Status st = p.Migrate(ro, 0, 1);
    EXPECT_FALSE(st.ok());
    EXPECT_NE(st.message().find("CX_ERR_SCHEMA_MIGRATION_FAILED"), std::string::npos);
    sqlite3_close(ro);
    std::remove(path.c_str());
}

TEST(ParentChildSchemaProviderTest, IndexNameCollisionFailsExec) {
    // parents + the blocks ALTERs succeed, but a pre-existing TABLE named
    // idx_blocks_child_id collides with the CREATE UNIQUE INDEX IF NOT EXISTS of
    // the same name (IF NOT EXISTS only suppresses a same-named index), so that
    // index Exec returns an error — exercising a later error-return branch.
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    CreateBlocksTable(db);
    ASSERT_EQ(sqlite3_exec(db, "CREATE TABLE idx_blocks_child_id(x INTEGER)",
                           nullptr, nullptr, nullptr), SQLITE_OK);
    ParentChildSchemaProvider p;
    Status st = p.Migrate(db, 0, 1);
    EXPECT_FALSE(st.ok());
    EXPECT_NE(st.message().find("CX_ERR_SCHEMA_MIGRATION_FAILED"), std::string::npos);
    // The columns were still added before the index step failed.
    EXPECT_TRUE(HasColumn(db, "blocks", "child_id"));
    sqlite3_close(db);
}

TEST(ParentChildSchemaProviderTest, PreexistingChildColumnsTakeSkipArm) {
    // When blocks already carries all 4 child columns, each
    // AddBlocksColumnIfAbsent hits the ColumnExists==true skip arm (no ALTER) and
    // Migrate still succeeds + builds the indexes.
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(db,
        "CREATE TABLE blocks (block_id INTEGER PRIMARY KEY AUTOINCREMENT, "
        "doc_id TEXT, chunk_index INTEGER, block_type INTEGER, content_text TEXT, "
        "child_id TEXT, parent_id TEXT, token_count INTEGER, parent_offset INTEGER)",
        nullptr, nullptr, nullptr), SQLITE_OK);
    ParentChildSchemaProvider p;
    Status st = p.Migrate(db, 0, 1);
    EXPECT_TRUE(st.ok()) << st.message();
    EXPECT_TRUE(HasColumn(db, "blocks", "child_id"));
    EXPECT_TRUE(HasIndex(db, "idx_blocks_child_id"));
    EXPECT_TRUE(HasTable(db, "parents"));
    sqlite3_close(db);
}

TEST(ParentChildSchemaProviderTest, ParentIndexNameCollisionFailsExec) {
    // child_id index succeeds; a pre-existing TABLE named idx_blocks_parent makes
    // the parent reverse-lookup index's Exec fail — a distinct later error-return
    // branch from the child_id collision above.
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    CreateBlocksTable(db);
    ASSERT_EQ(sqlite3_exec(db, "CREATE TABLE idx_blocks_parent(x INTEGER)",
                           nullptr, nullptr, nullptr), SQLITE_OK);
    ParentChildSchemaProvider p;
    Status st = p.Migrate(db, 0, 1);
    EXPECT_FALSE(st.ok());
    EXPECT_NE(st.message().find("CX_ERR_SCHEMA_MIGRATION_FAILED"), std::string::npos);
    EXPECT_TRUE(HasIndex(db, "idx_blocks_child_id"));  // earlier index did get built
    sqlite3_close(db);
}

TEST(ParentChildSchemaProviderTest, MetaDocIndexNameCollisionFailsExec) {
    // child_id + parent indexes succeed; a pre-existing TABLE named
    // idx_blocks_meta_doc makes the final META-uniqueness index Exec fail (the
    // last index error-return branch).
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    CreateBlocksTable(db);
    ASSERT_EQ(sqlite3_exec(db, "CREATE TABLE idx_blocks_meta_doc(x INTEGER)",
                           nullptr, nullptr, nullptr), SQLITE_OK);
    ParentChildSchemaProvider p;
    Status st = p.Migrate(db, 0, 1);
    EXPECT_FALSE(st.ok());
    EXPECT_NE(st.message().find("CX_ERR_SCHEMA_MIGRATION_FAILED"), std::string::npos);
    EXPECT_TRUE(HasIndex(db, "idx_blocks_child_id"));
    EXPECT_TRUE(HasIndex(db, "idx_blocks_parent"));
    sqlite3_close(db);
}

// Runs through the real SchemaMigrator per-Unit path (the catalog integration point).
TEST(ParentChildSchemaProviderTest, RegistersAndMigratesViaMigratorUnit) {
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    CreateBlocksTable(db);
    ParentChildSchemaProvider p;
    cortrix::catalog::SchemaMigrator m;
    m.Register(&p);
    Status st = m.MigrateUnit(db, "unit-1");
    ASSERT_TRUE(st.ok()) << st.message();
    EXPECT_EQ(m.CurrentVersion(db, "parent_child"), 1);
    EXPECT_TRUE(HasColumn(db, "blocks", "child_id"));
    EXPECT_TRUE(HasTable(db, "parents"));
    sqlite3_close(db);
}

}  // namespace
}  // namespace cortrix::store
