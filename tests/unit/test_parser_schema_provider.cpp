#include <gtest/gtest.h>

#include <string>

#include <sqlite3.h>

#include "cortrix/catalog/schema_provider.h"
#include "cortrix/spc/parser_schema_provider.h"

// Parser (Major-3): F06SchemaProvider adds namespaces.parser_config JSONB and
// registers with the catalog SchemaMigrator. Idempotent.
namespace cortrix::spc {
namespace {

// Minimal stand-in for catalog's `namespaces` table (only the shape the migration
// touches matters here — standalone, no full catalog schema).
void CreateNamespacesTable(sqlite3* db) {
    ASSERT_EQ(sqlite3_exec(db,
        "CREATE TABLE namespaces (ns_id TEXT PRIMARY KEY, name TEXT)",
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

TEST(F06SchemaProviderTest, IdentityAndVersion) {
    F06SchemaProvider p;
    EXPECT_EQ(p.FeatureName(), "F06");
    EXPECT_EQ(p.CurrentVersion(), 1);
}

TEST(F06SchemaProviderTest, Catalog_AddsParserConfigColumn) {
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    CreateNamespacesTable(db);
    EXPECT_FALSE(HasColumn(db, "namespaces", "parser_config"));

    F06SchemaProvider p;
    ASSERT_TRUE(p.Migrate(db, 0, 1).ok());
    EXPECT_TRUE(HasColumn(db, "namespaces", "parser_config"));

    // Default is '{}' for an inserted row that doesn't set it.
    ASSERT_EQ(sqlite3_exec(db, "INSERT INTO namespaces(ns_id, name) VALUES('n1','x')",
                           nullptr, nullptr, nullptr), SQLITE_OK);
    sqlite3_stmt* stmt = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(
        db, "SELECT parser_config FROM namespaces WHERE ns_id='n1'", -1, &stmt, nullptr),
        SQLITE_OK);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    EXPECT_STREQ(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)), "{}");
    sqlite3_finalize(stmt);
    sqlite3_close(db);
}

TEST(F06SchemaProviderTest, Migration_Idempotent) {
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    CreateNamespacesTable(db);
    F06SchemaProvider p;
    ASSERT_TRUE(p.Migrate(db, 0, 1).ok());
    // Re-running (and the already-current call) must not error or duplicate.
    EXPECT_TRUE(p.Migrate(db, 0, 1).ok());
    EXPECT_TRUE(p.Migrate(db, 1, 1).ok());
    EXPECT_TRUE(HasColumn(db, "namespaces", "parser_config"));
    sqlite3_close(db);
}

TEST(F06SchemaProviderTest, NoNamespacesTable_NoOp) {
    // If the catalog table isn't built yet (isolated test), migrate is a no-op,
    // not a failure — so the migrator batch isn't blocked.
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    F06SchemaProvider p;
    EXPECT_TRUE(p.Migrate(db, 0, 1).ok());
    sqlite3_close(db);
}

TEST(F06SchemaProviderTest, UnexpectedVersionStepIsError) {
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    F06SchemaProvider p;
    Status st = p.Migrate(db, 1, 2);  // no Phase 2 bump yet
    EXPECT_FALSE(st.ok());
    EXPECT_NE(st.message().find("CX_ERR_SCHEMA_VERSION_MISMATCH"), std::string::npos);
    sqlite3_close(db);
}

// Runs through the real SchemaMigrator (the catalog integration point).
TEST(F06SchemaProviderTest, RegistersAndMigratesViaMigrator) {
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    CreateNamespacesTable(db);
    F06SchemaProvider p;
    cortrix::catalog::SchemaMigrator m;
    m.Register(&p);
    Status st = m.MigrateCatalog(db);
    ASSERT_TRUE(st.ok()) << st.message();
    EXPECT_EQ(m.CurrentVersion(db, "F06"), 1);
    EXPECT_TRUE(HasColumn(db, "namespaces", "parser_config"));
    sqlite3_close(db);
}

}  // namespace
}  // namespace cortrix::spc
