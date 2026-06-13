#include "cortrix/spc_enricher/f35_schema_provider.h"

#include <gtest/gtest.h>

#include <string>

#include <sqlite3.h>

#include "cortrix/catalog/schema_provider.h"

// F35 § 4.1 (A unified-blocks, Derek 2026-06-07): F35SchemaProvider extends the
// per-Unit blocks table with the 4 contextual-retrieval columns (embedding,
// contextualized_text, contextualized_embedding, contextualized_status),
// registered via the F12 SchemaMigrator.
namespace cortrix::spc {
namespace {

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
        db, "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?1", -1, &stmt, nullptr),
        SQLITE_OK);
    sqlite3_bind_text(stmt, 1, table, -1, SQLITE_STATIC);
    const bool found = (sqlite3_step(stmt) == SQLITE_ROW);
    sqlite3_finalize(stmt);
    return found;
}

TEST(F35SchemaProviderTest, IdentityAndVersion) {
    F35SchemaProvider p;
    EXPECT_EQ(p.FeatureName(), "F35");
    EXPECT_EQ(p.CurrentVersion(), 1);
}

TEST(F35SchemaProviderTest, AddsFourContextualColumns) {
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    CreateBlocksTable(db);
    EXPECT_FALSE(HasColumn(db, "blocks", "contextualized_text"));

    F35SchemaProvider p;
    ASSERT_TRUE(p.Migrate(db, 0, 1).ok());

    EXPECT_TRUE(HasColumn(db, "blocks", "embedding"));
    EXPECT_TRUE(HasColumn(db, "blocks", "contextualized_text"));
    EXPECT_TRUE(HasColumn(db, "blocks", "contextualized_embedding"));
    EXPECT_TRUE(HasColumn(db, "blocks", "contextualized_status"));
    sqlite3_close(db);
}

TEST(F35SchemaProviderTest, MigrationIdempotent) {
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    CreateBlocksTable(db);
    F35SchemaProvider p;
    ASSERT_TRUE(p.Migrate(db, 0, 1).ok());
    EXPECT_TRUE(p.Migrate(db, 0, 1).ok());  // re-run
    EXPECT_TRUE(p.Migrate(db, 1, 1).ok());  // already current
    EXPECT_TRUE(HasColumn(db, "blocks", "embedding"));
    sqlite3_close(db);
}

TEST(F35SchemaProviderTest, NoBlocksTableNoOp) {
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    F35SchemaProvider p;
    EXPECT_TRUE(p.Migrate(db, 0, 1).ok());  // no blocks → no-op, not error
    EXPECT_FALSE(HasTable(db, "blocks"));
    sqlite3_close(db);
}

TEST(F35SchemaProviderTest, UnexpectedVersionStepIsError) {
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    F35SchemaProvider p;
    Status st = p.Migrate(db, 1, 2);  // no Phase 2 bump yet
    EXPECT_FALSE(st.ok());
    EXPECT_NE(st.message().find("CX_ERR_SCHEMA_VERSION_MISMATCH"), std::string::npos);
    sqlite3_close(db);
}

TEST(F35SchemaProviderTest, RegistersAndMigratesViaMigratorUnit) {
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    CreateBlocksTable(db);
    F35SchemaProvider p;
    cortrix::catalog::SchemaMigrator m;
    m.Register(&p);
    Status st = m.MigrateUnit(db, "unit-1");
    ASSERT_TRUE(st.ok()) << st.message();
    EXPECT_EQ(m.CurrentVersion(db, "F35"), 1);
    EXPECT_TRUE(HasColumn(db, "blocks", "contextualized_text"));
    sqlite3_close(db);
}

}  // namespace
}  // namespace cortrix::spc
