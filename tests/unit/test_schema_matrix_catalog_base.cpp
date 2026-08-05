// Schema-provider matrix for the catalog base-schema provider:
//   Catalog (CatalogSchemaProvider, catalog.db framework: tenants/namespaces/units/users/...,
//        CurrentVersion 2, version-agnostic Migrate that creates from scratch).
//
// Fresh in-memory sqlite3 per case. Globally unique suite/fixture names
// (CatalogCatMatrix / CatalogCatVersionMatrix) that do NOT reuse CatalogSchemaTest /
// BootstrapRowsTest from test_catalog_schema.cpp.

#include <gtest/gtest.h>

#include <sqlite3.h>

#include <string>

#include "cortrix/catalog/catalog_schema.h"

namespace cortrix::test_schema_matrix_catalog {
namespace {

struct SqliteHelpers {
    sqlite3* db = nullptr;
    void Open() { ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK); }
    void Close() { sqlite3_close(db); }
    bool ColumnExists(const char* table, const char* column) {
        std::string sql = std::string("SELECT 1 FROM pragma_table_info('") + table +
                          "') WHERE name='" + column + "';";
        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
        bool found = (sqlite3_step(stmt) == SQLITE_ROW);
        sqlite3_finalize(stmt);
        return found;
    }
    bool TableExists(const std::string& name) {
        std::string sql = "SELECT 1 FROM sqlite_master WHERE type='table' AND name='" + name + "';";
        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
        bool found = (sqlite3_step(stmt) == SQLITE_ROW);
        sqlite3_finalize(stmt);
        return found;
    }
};

class CatalogCatMatrix : public ::testing::Test, protected SqliteHelpers {
protected:
    void SetUp() override { Open(); }
    void TearDown() override { Close(); }
    cortrix::catalog::CatalogSchemaProvider p_;
};

TEST_F(CatalogCatMatrix, FeatureIdentity) {
    EXPECT_EQ(p_.FeatureName(), "catalog");
    EXPECT_EQ(p_.CurrentVersion(), 2);
    EXPECT_EQ(cortrix::catalog::kCatalogSchemaVersion, 2);
}

TEST_F(CatalogCatMatrix, MigrateCreatesCoreCatalogTables) {
    ASSERT_TRUE(p_.Migrate(db, 0, 1).ok());
    EXPECT_TRUE(TableExists("tenants"));
    EXPECT_TRUE(TableExists("namespaces"));
    EXPECT_TRUE(TableExists("units"));
    EXPECT_TRUE(TableExists("ns_units"));
}

TEST_F(CatalogCatMatrix, MigrateCreatesAuthAndAclTables) {
    ASSERT_TRUE(p_.Migrate(db, 0, 1).ok());
    EXPECT_TRUE(TableExists("users"));
    EXPECT_TRUE(TableExists("user_tenants"));
    EXPECT_TRUE(TableExists("ns_acl"));
    EXPECT_TRUE(TableExists("cross_tenant_whitelist"));
}

TEST_F(CatalogCatMatrix, MigrateCreatesContentAndGcTables) {
    ASSERT_TRUE(p_.Migrate(db, 0, 1).ok());
    EXPECT_TRUE(TableExists("content_refs"));
    EXPECT_TRUE(TableExists("blob_gc_queue"));
    EXPECT_TRUE(TableExists("file_locations"));
    EXPECT_TRUE(TableExists("nodes"));
}

TEST_F(CatalogCatMatrix, TenantsAndNamespacesShape) {
    ASSERT_TRUE(p_.Migrate(db, 0, 1).ok());
    EXPECT_TRUE(ColumnExists("tenants", "tenant_id"));
    EXPECT_TRUE(ColumnExists("namespaces", "ns_id"));
}

TEST_F(CatalogCatMatrix, BootstrapRowsSeeded) {
    ASSERT_TRUE(p_.Migrate(db, 0, 1).ok());
    // The DDL seeds a system tenant via INSERT OR IGNORE.
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, "SELECT 1 FROM tenants WHERE tenant_id='__system__';", -1, &stmt, nullptr);
    EXPECT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    sqlite3_finalize(stmt);
}

TEST_F(CatalogCatMatrix, IdempotentReRun) {
    ASSERT_TRUE(p_.Migrate(db, 0, 1).ok());
    EXPECT_TRUE(p_.Migrate(db, 0, 1).ok());
    EXPECT_TRUE(p_.Migrate(db, 1, 2).ok());
    EXPECT_TRUE(p_.Migrate(db, 2, 2).ok());
}

TEST_F(CatalogCatMatrix, BlobGcQueueHasBlobUriShape) {
    ASSERT_TRUE(p_.Migrate(db, 0, 1).ok());
    EXPECT_TRUE(ColumnExists("blob_gc_queue", "blob_uri"));
}

// Catalog's Migrate is version-agnostic: it creates the full catalog from scratch for any
// (from,to). Matrix confirms every step succeeds and seeds the catalog tables.
struct CatStep {
    int from;
    int to;
};
class CatalogCatVersionMatrix : public ::testing::TestWithParam<CatStep> {
protected:
    void SetUp() override { ASSERT_EQ(sqlite3_open(":memory:", &db_), SQLITE_OK); }
    void TearDown() override { sqlite3_close(db_); }
    sqlite3* db_ = nullptr;
    cortrix::catalog::CatalogSchemaProvider p_;
};
TEST_P(CatalogCatVersionMatrix, AlwaysCreatesCatalog) {
    const CatStep step = GetParam();
    Status s = p_.Migrate(db_, step.from, step.to);
    EXPECT_TRUE(s.ok());
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_, "SELECT 1 FROM sqlite_master WHERE type='table' AND name='namespaces';",
                       -1, &stmt, nullptr);
    EXPECT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    sqlite3_finalize(stmt);
}
INSTANTIATE_TEST_SUITE_P(
    CatalogCatSteps, CatalogCatVersionMatrix,
    ::testing::Values(CatStep{0, 1}, CatStep{0, 2}, CatStep{1, 1}, CatStep{1, 2},
                      CatStep{2, 2}, CatStep{0, 0}, CatStep{3, 3}, CatStep{0, 3},
                      CatStep{2, 1}));

}  // namespace
}  // namespace cortrix::test_schema_matrix_catalog
