// catalog_schema.cpp supplemental tests — targets branches at 69.2% line coverage.
// Focuses on:
//   - blob_gc_queue clean-break migration (stale schema → drop → recreate)
//   - kCatalogSchemaSql idempotency (CREATE TABLE IF NOT EXISTS re-runs)
//   - Bootstrap rows (default_tenant, default_user, __system__ tenant)
//   - blob_gc_queue authoritative column set (blob_uri PK, 6 columns)
//   - CatalogSchemaProvider Migrate on db with stale blob_gc_queue shape
//   - CatalogSchemaProvider FeatureName / CurrentVersion accessors
//   - catalog_schema SQL applied directly (without CatalogDb wrapper)
#include <gtest/gtest.h>

#include <sqlite3.h>
#include <set>
#include <string>
#include <vector>

#include "cortrix/catalog/catalog_db.h"
#include "cortrix/catalog/catalog_schema.h"
#include "cortrix/common/status.h"

namespace cortrix::catalog {
namespace {

// ---------------------------------------------------------------------------
// Test-local SQLite query helpers (mirrors test_catalog_schema.cpp, kept
// local so this file compiles without a common test header).
// ---------------------------------------------------------------------------

std::set<std::string> ColSet(sqlite3* db, const std::string& table) {
    std::set<std::string> out;
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db,
        ("SELECT name FROM pragma_table_info('" + table + "')").c_str(),
        -1, &stmt, nullptr);
    while (stmt && sqlite3_step(stmt) == SQLITE_ROW) {
        const char* t = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        if (t) out.insert(t);
    }
    sqlite3_finalize(stmt);
    return out;
}

int64_t QInt(sqlite3* db, const std::string& sql) {
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    int64_t v = 0;
    if (stmt && sqlite3_step(stmt) == SQLITE_ROW)
        v = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return v;
}

std::string QStr(sqlite3* db, const std::string& sql) {
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    std::string v;
    if (stmt && sqlite3_step(stmt) == SQLITE_ROW) {
        const char* t = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        if (t) v = t;
    }
    sqlite3_finalize(stmt);
    return v;
}

bool TableExists(sqlite3* db, const std::string& table) {
    return QInt(db,
        "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='" + table + "'") > 0;
}

// ---------------------------------------------------------------------------
// CatalogSchemaProvider::Migrate — clean-break of stale blob_gc_queue.
// The Migrate() code detects whether blob_gc_queue lacks the `blob_uri` column;
// if it does, the stale table is dropped and the authoritative DDL recreates it.
// ---------------------------------------------------------------------------

// When blob_gc_queue has a stale schema (missing blob_uri), Migrate drops and
// recreates it with the correct 6-column shape.
TEST(CatalogSchemaMigrateCleanBreakTest, StaleBlobGcQueueIsDroppedAndRecreated) {
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);

    // Create the stale shape (pre-OPEN-2 columns — no blob_uri PK).
    ASSERT_EQ(sqlite3_exec(db,
        "CREATE TABLE blob_gc_queue ("
        "  blob_id INTEGER PRIMARY KEY, stage TEXT, enqueued_at INTEGER, purge_after INTEGER"
        ")",
        nullptr, nullptr, nullptr), SQLITE_OK);
    // Verify stale shape exists.
    auto stale_cols = ColSet(db, "blob_gc_queue");
    EXPECT_EQ(stale_cols.count("blob_uri"), 0u) << "pre-condition: stale shape has no blob_uri";

    CatalogSchemaProvider provider;
    Status s = provider.Migrate(db, 0, provider.CurrentVersion());
    ASSERT_TRUE(s.ok()) << s.message();

    // blob_gc_queue now has blob_uri as the primary key column.
    auto new_cols = ColSet(db, "blob_gc_queue");
    EXPECT_GT(new_cols.count("blob_uri"), 0u) << "blob_uri missing after migration";
    // Stale columns are gone.
    EXPECT_EQ(new_cols.count("blob_id"), 0u)       << "stale blob_id survived migration";
    EXPECT_EQ(new_cols.count("enqueued_at"), 0u)   << "stale enqueued_at survived migration";
    EXPECT_EQ(new_cols.count("purge_after"), 0u)   << "stale purge_after survived migration";

    sqlite3_close(db);
}

// When blob_gc_queue already has blob_uri (correct schema), Migrate is a no-op
// (CREATE TABLE IF NOT EXISTS skips it).
TEST(CatalogSchemaMigrateCleanBreakTest, ExistingBlobUriSchemaIsPreserved) {
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);

    // Simulate an already-migrated DB: Migrate once to get the right shape.
    CatalogSchemaProvider provider;
    ASSERT_TRUE(provider.Migrate(db, 0, provider.CurrentVersion()).ok());
    // Insert a sentinel row into blob_gc_queue.
    ASSERT_EQ(sqlite3_exec(db,
        "INSERT INTO blob_gc_queue "
        "(blob_uri, file_hash, queued_at, eligible_at, status) "
        "VALUES ('sentinel://uri', 'fh1', 0, 0, 'pending')",
        nullptr, nullptr, nullptr), SQLITE_OK);

    // Run Migrate again (idempotent re-run).
    Status s2 = provider.Migrate(db, 0, provider.CurrentVersion());
    ASSERT_TRUE(s2.ok()) << s2.message();

    // Sentinel row must still be present — the table was NOT dropped.
    EXPECT_EQ(QInt(db,
        "SELECT COUNT(*) FROM blob_gc_queue WHERE blob_uri='sentinel://uri'"), 1);

    sqlite3_close(db);
}

// blob_gc_queue absent entirely (no table at all): no-table probe path — table_exists=false
// means the drop branch is skipped and the table is simply created fresh.
TEST(CatalogSchemaMigrateCleanBreakTest, MissingBlobGcQueueCreatedFresh) {
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    // No blob_gc_queue table created at all; just run Migrate.
    CatalogSchemaProvider provider;
    Status s = provider.Migrate(db, 0, provider.CurrentVersion());
    ASSERT_TRUE(s.ok()) << s.message();
    EXPECT_TRUE(TableExists(db, "blob_gc_queue"));
    auto cols = ColSet(db, "blob_gc_queue");
    EXPECT_GT(cols.count("blob_uri"), 0u);
    sqlite3_close(db);
}

// ---------------------------------------------------------------------------
// CatalogSchemaProvider metadata accessors.
// ---------------------------------------------------------------------------

TEST(CatalogSchemaProviderTest, FeatureNameIsF12) {
    CatalogSchemaProvider p;
    EXPECT_EQ(p.FeatureName(), "catalog");
}

TEST(CatalogSchemaProviderTest, CurrentVersionMatchesConstant) {
    CatalogSchemaProvider p;
    EXPECT_EQ(p.CurrentVersion(), kCatalogSchemaVersion);
}

// kCatalogSchemaVersion must be at least 2 (bumped from 1 in OPEN-2 GC reshape).
TEST(CatalogSchemaProviderTest, SchemaVersionAtLeastTwo) {
    EXPECT_GE(kCatalogSchemaVersion, 2);
}

// ---------------------------------------------------------------------------
// kCatalogSchemaSql idempotency: applying the DDL to a database that already
// has all the tables is a no-op (all objects have IF NOT EXISTS guards).
// ---------------------------------------------------------------------------

TEST(CatalogSchemaSqlTest, IdempotentApply_AlreadyMigratedDb) {
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);

    // Apply once.
    char* err = nullptr;
    ASSERT_EQ(sqlite3_exec(db, kCatalogSchemaSql, nullptr, nullptr, &err), SQLITE_OK)
        << (err ? err : "unknown error");
    sqlite3_free(err);

    // Apply again — must not fail.
    err = nullptr;
    int rc = sqlite3_exec(db, kCatalogSchemaSql, nullptr, nullptr, &err);
    EXPECT_EQ(rc, SQLITE_OK) << (err ? err : "");
    sqlite3_free(err);

    sqlite3_close(db);
}

// ---------------------------------------------------------------------------
// Bootstrap rows: default_tenant / default_user / __system__ tenant inserted
// by the OSS bootstrap block (INSERT OR IGNORE).
// ---------------------------------------------------------------------------

class BootstrapRowsTest : public ::testing::Test {
protected:
    void SetUp() override {
        Status st = catalog_.Open(":memory:");
        ASSERT_TRUE(st.ok()) << st.message();
        db_ = catalog_.db();
    }
    CatalogDb catalog_;
    sqlite3* db_ = nullptr;
};

TEST_F(BootstrapRowsTest, DefaultTenantExists) {
    EXPECT_EQ(QInt(db_,
        "SELECT COUNT(*) FROM tenants WHERE tenant_id='default_tenant'"), 1);
}

TEST_F(BootstrapRowsTest, DefaultTenantType) {
    EXPECT_EQ(QStr(db_,
        "SELECT type FROM tenants WHERE tenant_id='default_tenant'"), "personal");
}

TEST_F(BootstrapRowsTest, SystemTenantExists) {
    EXPECT_EQ(QInt(db_,
        "SELECT COUNT(*) FROM tenants WHERE tenant_id='__system__'"), 1);
}

TEST_F(BootstrapRowsTest, SystemTenantType) {
    EXPECT_EQ(QStr(db_,
        "SELECT type FROM tenants WHERE tenant_id='__system__'"), "system");
}

TEST_F(BootstrapRowsTest, DefaultUserExists) {
    EXPECT_EQ(QInt(db_,
        "SELECT COUNT(*) FROM users WHERE user_id='default_user'"), 1);
}

TEST_F(BootstrapRowsTest, DefaultUserTenantOwnerRelationship) {
    EXPECT_EQ(QStr(db_,
        "SELECT role FROM user_tenants "
        "WHERE user_id='default_user' AND tenant_id='default_tenant'"), "owner");
}

// INSERT OR IGNORE: a second Open() (re-migration) must not produce duplicate bootstrap rows.
TEST_F(BootstrapRowsTest, BootstrapRowsIdempotent) {
    // Run Migrate again directly.
    CatalogSchemaProvider p;
    ASSERT_TRUE(p.Migrate(db_, 0, p.CurrentVersion()).ok());
    EXPECT_EQ(QInt(db_, "SELECT COUNT(*) FROM tenants WHERE tenant_id='default_tenant'"), 1);
    EXPECT_EQ(QInt(db_, "SELECT COUNT(*) FROM users WHERE user_id='default_user'"), 1);
    EXPECT_EQ(QInt(db_, "SELECT COUNT(*) FROM tenants WHERE tenant_id='__system__'"), 1);
}

// ---------------------------------------------------------------------------
// blob_gc_queue authoritative column set after migration.
// ---------------------------------------------------------------------------

TEST(BlobGcQueueSchemaTest, HasAllSixColumns) {
    CatalogDb catalog;
    ASSERT_TRUE(catalog.Open(":memory:").ok());
    auto cols = ColSet(catalog.db(), "blob_gc_queue");
    for (const char* c : {"blob_uri", "file_hash", "queued_at", "eligible_at",
                          "last_ref_check", "status"}) {
        EXPECT_GT(cols.count(c), 0u) << "blob_gc_queue missing column: " << c;
    }
}

// blob_uri must be the PRIMARY KEY (only column in pk_list).
TEST(BlobGcQueueSchemaTest, BlobUriIsPrimaryKey) {
    CatalogDb catalog;
    ASSERT_TRUE(catalog.Open(":memory:").ok());
    sqlite3* db = catalog.db();
    // PRAGMA table_info returns pk != 0 for PK columns.
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, "SELECT name, pk FROM pragma_table_info('blob_gc_queue')",
                       -1, &stmt, nullptr);
    std::string pk_col;
    while (stmt && sqlite3_step(stmt) == SQLITE_ROW) {
        int pk = sqlite3_column_int(stmt, 1);
        if (pk > 0) {
            const char* t = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            if (t) pk_col = t;
        }
    }
    sqlite3_finalize(stmt);
    EXPECT_EQ(pk_col, "blob_uri");
}

// The eligible_at+status index exists (used by Stage 3 query).
TEST(BlobGcQueueSchemaTest, EligibleAtIndexExists) {
    CatalogDb catalog;
    ASSERT_TRUE(catalog.Open(":memory:").ok());
    EXPECT_EQ(QInt(catalog.db(),
        "SELECT COUNT(*) FROM sqlite_master WHERE type='index' "
        "AND name='idx_gc_eligible'"), 1);
}

// ---------------------------------------------------------------------------
// content_refs index (idx_cr_ns) must exist.
// ---------------------------------------------------------------------------

TEST(ContentRefsSchemaTest, NsIndexExists) {
    CatalogDb catalog;
    ASSERT_TRUE(catalog.Open(":memory:").ok());
    EXPECT_EQ(QInt(catalog.db(),
        "SELECT COUNT(*) FROM sqlite_master WHERE type='index' "
        "AND name='idx_cr_ns'"), 1);
}

// ---------------------------------------------------------------------------
// nodes table: single 'local' row with hostname 'localhost'.
// ---------------------------------------------------------------------------

TEST(NodesSchemaTest, LocalNodeHostname) {
    CatalogDb catalog;
    ASSERT_TRUE(catalog.Open(":memory:").ok());
    EXPECT_EQ(QStr(catalog.db(),
        "SELECT hostname FROM nodes WHERE node_id='local'"), "localhost");
}

// ---------------------------------------------------------------------------
// namespaces: clone-related columns exist (cloned_from_ns_id, clone_type,
// cloned_at, clone_lineage).
// ---------------------------------------------------------------------------

TEST(NamespacesSchemaTest, CloneColumnsPresent) {
    CatalogDb catalog;
    ASSERT_TRUE(catalog.Open(":memory:").ok());
    auto cols = ColSet(catalog.db(), "namespaces");
    for (const char* c : {"cloned_from_ns_id", "clone_type", "cloned_at", "clone_lineage"}) {
        EXPECT_GT(cols.count(c), 0u) << "namespaces missing clone column: " << c;
    }
}

}  // namespace
}  // namespace cortrix::catalog
