// Doc summary S0 — DocSummarySchemaProvider creates the net-new doc_fts5_index FTS5 virtual
// table inside the catalog SchemaMigrator framework. Pins identity/version,
// the virtual-table existence + columns after migration, idempotency, the
// bad-version-step guard, and SchemaMigrator registration. Mirrors
// test_sparse_schema_provider.cpp.
#include <gtest/gtest.h>

#include <sqlite3.h>

#include <set>
#include <string>

#include "cortrix/catalog/schema_provider.h"
#include "cortrix/doc_summary/doc_summary_schema_provider.h"

namespace cortrix::doc_summary {
namespace {

std::set<std::string> TableNames(sqlite3* db) {
    std::set<std::string> out;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT name FROM sqlite_master WHERE type='table'",
                           -1, &st, nullptr) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            const char* t = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
            if (t) out.insert(t);
        }
    }
    sqlite3_finalize(st);
    return out;
}

std::set<std::string> ColumnNames(sqlite3* db, const std::string& table) {
    std::set<std::string> out;
    sqlite3_stmt* st = nullptr;
    std::string sql = "SELECT name FROM pragma_table_info('" + table + "')";
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            const char* t = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
            if (t) out.insert(t);
        }
    }
    sqlite3_finalize(st);
    return out;
}

TEST(DocSummarySchemaProviderTest, IdentityAndVersion) {
    DocSummarySchemaProvider p;
    EXPECT_EQ(p.FeatureName(), "doc_summary");
    EXPECT_EQ(p.CurrentVersion(), 1);
}

TEST(DocSummarySchemaProviderTest, MigrateCreatesVirtualTableAndColumns) {
    DocSummarySchemaProvider p;
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    ASSERT_TRUE(p.Migrate(db, 0, 1).ok());

    EXPECT_TRUE(TableNames(db).count("doc_fts5_index"));
    auto cols = ColumnNames(db, "doc_fts5_index");
    EXPECT_TRUE(cols.count("doc_id"));
    EXPECT_TRUE(cols.count("filename"));
    EXPECT_TRUE(cols.count("doc_title"));
    EXPECT_TRUE(cols.count("topics_rule_extracted"));
    EXPECT_TRUE(cols.count("authors"));
    sqlite3_close(db);
}

TEST(DocSummarySchemaProviderTest, MigrateIsIdempotent) {
    DocSummarySchemaProvider p;
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    EXPECT_TRUE(p.Migrate(db, 0, 1).ok());
    EXPECT_TRUE(p.Migrate(db, 0, 1).ok());  // re-run → IF NOT EXISTS, ok
    EXPECT_TRUE(p.Migrate(db, 1, 1).ok());  // already current
    EXPECT_TRUE(TableNames(db).count("doc_fts5_index"));
    sqlite3_close(db);
}

TEST(DocSummarySchemaProviderTest, UnexpectedVersionStepIsError) {
    DocSummarySchemaProvider p;
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    Status st = p.Migrate(db, 1, 2);  // Phase 2 not implemented
    EXPECT_FALSE(st.ok());
    EXPECT_NE(st.message().find("CX_ERR_DOCSUMMARY_SCHEMA_VERSION_MISMATCH"),
              std::string::npos);
    sqlite3_close(db);
}

TEST(DocSummarySchemaProviderTest, NullDbRejected) {
    DocSummarySchemaProvider p;
    Status st = p.Migrate(nullptr, 0, 1);
    EXPECT_FALSE(st.ok());
    EXPECT_NE(st.message().find("CX_ERR_DOCSUMMARY_SCHEMA_VERSION_MISMATCH"),
              std::string::npos);
}

TEST(DocSummarySchemaProviderTest, RegistersWithSchemaMigrator) {
    // Doc summary provider runs inside the catalog SchemaMigrator transaction
    // (the integrated path). Pin the closure: after MigrateUnit the table exists.
    DocSummarySchemaProvider p;
    cortrix::catalog::SchemaMigrator migrator;
    migrator.Register(&p);
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    ASSERT_TRUE(migrator.MigrateUnit(db, "unit_test").ok());
    EXPECT_TRUE(TableNames(db).count("doc_fts5_index"));
    EXPECT_EQ(migrator.CurrentVersion(db, "doc_summary"), 1);
    sqlite3_close(db);
}

}  // namespace
}  // namespace cortrix::doc_summary
