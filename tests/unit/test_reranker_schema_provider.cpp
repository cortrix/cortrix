// S4.1 — RerankerSchemaProvider registers with the catalog SchemaMigrator for
// namespaces.reranker_config version coordination (reranker / Issue 2.3).
// reranker_config is one of the 11 standardized *_config JSONB columns supplied
// by the catalog base schema, so reranker's Phase-1 Migrate is a no-op. These tests pin
// that contract + the reranker <-> catalog closure (the column really exists after a
// combined migration). Mirrors test_watcher_schema_provider.cpp.
#include <gtest/gtest.h>

#include <sqlite3.h>

#include <set>
#include <string>

#include "cortrix/catalog/catalog_schema.h"
#include "cortrix/catalog/schema_provider.h"
#include "cortrix/reranker/reranker_schema_provider.h"

namespace cortrix::reranker {
namespace {

std::set<std::string> ColumnNames(sqlite3* db, const std::string& table) {
    std::set<std::string> out;
    sqlite3_stmt* stmt = nullptr;
    std::string sql = "SELECT name FROM pragma_table_info('" + table + "')";
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char* t = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            if (t) out.insert(t);
        }
    }
    sqlite3_finalize(stmt);
    return out;
}

TEST(RerankerSchemaProviderTest, IdentityAndVersion) {
    RerankerSchemaProvider p;
    EXPECT_EQ(p.FeatureName(), "reranker");
    EXPECT_EQ(p.CurrentVersion(), 1);
}

TEST(RerankerSchemaProviderTest, Phase1MigrateIsNoOp) {
    RerankerSchemaProvider p;
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    EXPECT_TRUE(p.Migrate(db, 0, 1).ok());  // init no-op
    EXPECT_TRUE(p.Migrate(db, 1, 1).ok());  // already current
    sqlite3_close(db);
}

TEST(RerankerSchemaProviderTest, UnexpectedVersionStepIsError) {
    RerankerSchemaProvider p;
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    Status st = p.Migrate(db, 1, 2);  // Phase 2 not implemented yet
    EXPECT_FALSE(st.ok());
    EXPECT_NE(st.message().find("CX_ERR_SCHEMA_VERSION_MISMATCH"), std::string::npos);
    sqlite3_close(db);
}

TEST(RerankerSchemaProviderTest, RegistersAndMigratesViaMigrator) {
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    RerankerSchemaProvider p;
    cortrix::catalog::SchemaMigrator m;
    m.Register(&p);
    Status st = m.MigrateCatalog(db);
    ASSERT_TRUE(st.ok()) << st.message();
    EXPECT_EQ(m.CurrentVersion(db, "reranker"), 1);  // recorded at v1
    sqlite3_close(db);
}

// Reranker <-> catalog closure: the reranker_config column reranker relies on is provided by
// the frozen catalog base schema. Register catalog first (topological order), then reranker,
// run a single atomic migration, and assert the column exists with both versions.
TEST(RerankerSchemaProviderTest, RerankerConfigColumnSuppliedByCatalogBaseSchema) {
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);

    cortrix::catalog::CatalogSchemaProvider f12;
    RerankerSchemaProvider f02;
    cortrix::catalog::SchemaMigrator m;
    m.Register(&f12);  // Catalog first (ARCH §1.3.bis.3 topological order)
    m.Register(&f02);

    Status st = m.MigrateCatalog(db);
    ASSERT_TRUE(st.ok()) << st.message();

    auto cols = ColumnNames(db, "namespaces");
    EXPECT_NE(cols.find("reranker_config"), cols.end())
        << "namespaces.reranker_config must be present (F12 base schema)";

    EXPECT_EQ(m.CurrentVersion(db, "catalog"), cortrix::catalog::kCatalogSchemaVersion);
    EXPECT_EQ(m.CurrentVersion(db, "reranker"), 1);
    sqlite3_close(db);
}

}  // namespace
}  // namespace cortrix::reranker
