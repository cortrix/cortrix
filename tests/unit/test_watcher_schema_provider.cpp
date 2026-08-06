// Watcher: WatcherSchemaProvider registers with the catalog SchemaMigrator for
// namespaces.watcher_config version coordination. watcher_config is supplied by
// the catalog base schema (one of the 11 standardized *_config JSONB columns), so
// Watcher's Phase-1 Migrate is a no-op — these tests pin that contract and verify
// the watcher <-> catalog closure (the column really exists after a combined migration).
#include <gtest/gtest.h>

#include <sqlite3.h>

#include <set>
#include <string>

#include "cortrix/catalog/catalog_schema.h"
#include "cortrix/catalog/schema_provider.h"
#include "cortrix/connector/watcher_schema_provider.h"

namespace cortrix::connector {
namespace {

// Column names of `table` via PRAGMA table_info (cf. test_catalog_schema.cpp).
std::set<std::string> ColumnNames(sqlite3* db, const std::string& table) {
    std::set<std::string> out;
    sqlite3_stmt* stmt = nullptr;
    std::string sql = "SELECT name FROM pragma_table_info('" + table + "')";
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char* t =
                reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            if (t) out.insert(t);
        }
    }
    sqlite3_finalize(stmt);
    return out;
}

TEST(WatcherSchemaProviderTest, IdentityAndVersion) {
    WatcherSchemaProvider p;
    EXPECT_EQ(p.FeatureName(), "watcher");
    EXPECT_EQ(p.CurrentVersion(), 1);
}

TEST(WatcherSchemaProviderTest, Phase1MigrateIsNoOp) {
    WatcherSchemaProvider p;
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    EXPECT_TRUE(p.Migrate(db, 0, 1).ok());  // init no-op
    EXPECT_TRUE(p.Migrate(db, 1, 1).ok());  // already current
    sqlite3_close(db);
}

TEST(WatcherSchemaProviderTest, UnexpectedVersionStepIsError) {
    WatcherSchemaProvider p;
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    Status st = p.Migrate(db, 1, 2);  // Phase 2 not implemented yet
    EXPECT_FALSE(st.ok());
    EXPECT_NE(st.message().find("CX_ERR_SCHEMA_VERSION_MISMATCH"),
              std::string::npos);
    sqlite3_close(db);
}

// Runs through the real SchemaMigrator (the integration point watcher registers at).
TEST(WatcherSchemaProviderTest, RegistersAndMigratesViaMigrator) {
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    WatcherSchemaProvider p;
    cortrix::catalog::SchemaMigrator m;
    m.Register(&p);
    Status st = m.MigrateCatalog(db);
    ASSERT_TRUE(st.ok()) << st.message();
    EXPECT_EQ(m.CurrentVersion(db, "watcher"), 1);  // recorded at v1
    sqlite3_close(db);
}

// Watcher <-> catalog closure: the watcher_config column watcher relies on is provided by the
// frozen catalog base schema. Register catalog first (topological order), then watcher, run a
// single atomic migration, and assert the column exists with both versions set.
TEST(WatcherSchemaProviderTest, WatcherConfigColumnSuppliedByCatalogBaseSchema) {
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);

    cortrix::catalog::CatalogSchemaProvider catalog;
    WatcherSchemaProvider dir_watcher;
    cortrix::catalog::SchemaMigrator m;
    m.Register(&catalog);  // Catalog first (ARCH topological order)
    m.Register(&dir_watcher);

    Status st = m.MigrateCatalog(db);
    ASSERT_TRUE(st.ok()) << st.message();

    auto cols = ColumnNames(db, "namespaces");
    EXPECT_NE(cols.find("watcher_config"), cols.end())
        << "namespaces.watcher_config must be present (catalog base schema)";

    EXPECT_EQ(m.CurrentVersion(db, "catalog"), cortrix::catalog::kCatalogSchemaVersion);
    EXPECT_EQ(m.CurrentVersion(db, "watcher"), 1);
    sqlite3_close(db);
}

}  // namespace
}  // namespace cortrix::connector
