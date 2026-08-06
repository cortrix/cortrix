#include <gtest/gtest.h>

#include <sqlite3.h>

#include <set>
#include <string>
#include <vector>

#include "cortrix/catalog/catalog_db.h"
#include "cortrix/catalog/catalog_schema.h"

namespace cortrix::catalog {
namespace {

// ---- small sqlite query helpers (test-local) -------------------------------

// Collect column 0 (TEXT) of every row of `sql` into a set.
std::set<std::string> QueryTextSet(sqlite3* db, const std::string& sql) {
    std::set<std::string> out;
    sqlite3_stmt* stmt = nullptr;
    EXPECT_EQ(sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr), SQLITE_OK)
        << sqlite3_errmsg(db);
    while (stmt && sqlite3_step(stmt) == SQLITE_ROW) {
        const char* t = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        if (t) out.insert(t);
    }
    sqlite3_finalize(stmt);
    return out;
}

// Column names of `table` via PRAGMA table_info (name is column 1).
std::set<std::string> TableColumns(sqlite3* db, const std::string& table) {
    return QueryTextSet(db, "SELECT name FROM pragma_table_info('" + table + "')");
}

// Single-row, single-column INTEGER scalar query (e.g. COUNT(*)).
int64_t QueryInt(sqlite3* db, const std::string& sql) {
    sqlite3_stmt* stmt = nullptr;
    EXPECT_EQ(sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr), SQLITE_OK)
        << sqlite3_errmsg(db);
    int64_t v = -1;
    if (stmt && sqlite3_step(stmt) == SQLITE_ROW) v = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return v;
}

// Single-row, single-column TEXT scalar query.
std::string QueryText(sqlite3* db, const std::string& sql) {
    sqlite3_stmt* stmt = nullptr;
    EXPECT_EQ(sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr), SQLITE_OK)
        << sqlite3_errmsg(db);
    std::string v;
    if (stmt && sqlite3_step(stmt) == SQLITE_ROW) {
        const char* t = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        if (t) v = t;
    }
    sqlite3_finalize(stmt);
    return v;
}

bool Contains(const std::set<std::string>& s, const std::string& k) {
    return s.find(k) != s.end();
}

// Fixture: an in-memory catalog.db opened + migrated once per test.
class CatalogSchemaTest : public ::testing::Test {
protected:
    void SetUp() override {
        Status st = catalog_.Open(":memory:");
        ASSERT_TRUE(st.ok()) << st.message();
        db_ = catalog_.db();
        ASSERT_NE(db_, nullptr);
    }
    CatalogDb catalog_;
    sqlite3* db_ = nullptr;
};

// DoD: catalog.db opens and all 6 main + 6 association tables exist.
TEST_F(CatalogSchemaTest, AllTwelveTablesExist) {
    auto tables = QueryTextSet(
        db_, "SELECT name FROM sqlite_master WHERE type='table'");
    const std::vector<std::string> expected = {
        // 6 main
        "file_locations", "ns_units", "units", "nodes", "tenants", "namespaces",
        // 6 association
        "content_refs", "blob_gc_queue", "cross_tenant_whitelist",
        "users", "user_tenants", "ns_acl",
    };
    for (const auto& t : expected) {
        EXPECT_TRUE(Contains(tables, t)) << "missing table: " << t;
    }
    // schema_version bookkeeping table also present (created by the migrator).
    EXPECT_TRUE(Contains(tables, "schema_version"));
}

// DoD: the 9 reserved Phase 2 fields exist on units, plus the two seal_threshold
// columns; verify the full reserved set is present.
TEST_F(CatalogSchemaTest, UnitsHasNineReservedFields) {
    auto cols = TableColumns(db_, "units");
    for (const char* c : {"state", "index_type", "sealed_at", "archived_at",
                          "vector_count", "size_bytes", "last_write_at",
                          "seal_idle_days", "owner_node_id",
                          "seal_threshold_vectors", "seal_threshold_bytes"}) {
        EXPECT_TRUE(Contains(cols, c)) << "units missing column: " << c;
    }
}

// DoD: default values of the units reserved fields are correct. Insert a row
// supplying only the required (non-default) columns, then read back defaults.
TEST_F(CatalogSchemaTest, UnitsDefaultValuesCorrect) {
    // tenants row first (units.tenant_id FK → tenants).
    ASSERT_EQ(sqlite3_exec(db_,
        "INSERT INTO tenants(tenant_id, created_at) VALUES('t1', 0)",
        nullptr, nullptr, nullptr), SQLITE_OK) << sqlite3_errmsg(db_);
    ASSERT_EQ(sqlite3_exec(db_,
        "INSERT INTO units(unit_id, tenant_id, created_at) VALUES('u1','t1',0)",
        nullptr, nullptr, nullptr), SQLITE_OK) << sqlite3_errmsg(db_);

    EXPECT_EQ(QueryText(db_, "SELECT state FROM units WHERE unit_id='u1'"), "active");
    EXPECT_EQ(QueryText(db_, "SELECT index_type FROM units WHERE unit_id='u1'"), "hnsw");
    EXPECT_EQ(QueryText(db_, "SELECT owner_node_id FROM units WHERE unit_id='u1'"), "local");
    EXPECT_EQ(QueryInt(db_, "SELECT vector_count FROM units WHERE unit_id='u1'"), 0);
    EXPECT_EQ(QueryInt(db_, "SELECT size_bytes FROM units WHERE unit_id='u1'"), 0);
    EXPECT_EQ(QueryInt(db_, "SELECT last_write_at FROM units WHERE unit_id='u1'"), 0);
    // sealed_at / archived_at default NULL.
    EXPECT_EQ(QueryInt(db_,
        "SELECT COUNT(*) FROM units WHERE unit_id='u1' "
        "AND sealed_at IS NULL AND archived_at IS NULL"), 1);
    // seal thresholds default to INT64_MAX / INT32_MAX (Phase 1: never seal).
    EXPECT_EQ(QueryInt(db_, "SELECT seal_threshold_vectors FROM units WHERE unit_id='u1'"),
              9223372036854775807LL);
    EXPECT_EQ(QueryInt(db_, "SELECT seal_threshold_bytes FROM units WHERE unit_id='u1'"),
              9223372036854775807LL);
    EXPECT_EQ(QueryInt(db_, "SELECT seal_idle_days FROM units WHERE unit_id='u1'"),
              2147483647);
}

// file_locations carries the full 16-column schema (ARCH).
TEST_F(CatalogSchemaTest, FileLocationsSixteenColumns) {
    auto cols = TableColumns(db_, "file_locations");
    for (const char* c : {"file_hash", "unit_id", "ns_id", "tenant_id",
                          "size_bytes", "chunk_count", "blob_uri", "storage_type",
                          "ref_count", "isolated", "first_seen_at", "last_accessed",
                          "status", "deleted_at", "created_at"}) {
        EXPECT_TRUE(Contains(cols, c)) << "file_locations missing column: " << c;
    }
    EXPECT_EQ(cols.size(), 15u);  // 15 named columns (PK file_hash inclusive)
}

// namespaces carries all 11 *_config columns and the correct OPEN-1 defaults.
TEST_F(CatalogSchemaTest, NamespacesElevenConfigColumnsAndDefaults) {
    auto cols = TableColumns(db_, "namespaces");
    for (const char* c : {"reranker_config", "enricher_config", "parser_config",
                          "memory_config", "watcher_config", "cleaning_config",
                          "rag_fusion_config", "crag_config", "complexity_config",
                          "sparse_config", "doc_summary_config"}) {
        EXPECT_TRUE(Contains(cols, c)) << "namespaces missing *_config: " << c;
    }
    // structural columns present too.
    for (const char* c : {"name", "cloned_from_ns_id", "clone_type", "cloned_at",
                          "owner_user_id", "isolation_mode", "visibility", "status"}) {
        EXPECT_TRUE(Contains(cols, c)) << "namespaces missing column: " << c;
    }

    // Defaults: isolation_mode='shared', visibility='tenant', status='active',
    // every *_config defaults to '{}'.
    ASSERT_EQ(sqlite3_exec(db_,
        "INSERT INTO tenants(tenant_id, created_at) VALUES('t1', 0)",
        nullptr, nullptr, nullptr), SQLITE_OK) << sqlite3_errmsg(db_);
    ASSERT_EQ(sqlite3_exec(db_,
        "INSERT INTO namespaces(ns_id, tenant_id, created_at) VALUES('ns1','t1',0)",
        nullptr, nullptr, nullptr), SQLITE_OK) << sqlite3_errmsg(db_);
    EXPECT_EQ(QueryText(db_, "SELECT isolation_mode FROM namespaces WHERE ns_id='ns1'"),
              "shared");
    EXPECT_EQ(QueryText(db_, "SELECT visibility FROM namespaces WHERE ns_id='ns1'"),
              "tenant");
    EXPECT_EQ(QueryText(db_, "SELECT status FROM namespaces WHERE ns_id='ns1'"),
              "active");
    EXPECT_EQ(QueryText(db_, "SELECT reranker_config FROM namespaces WHERE ns_id='ns1'"),
              "{}");
    EXPECT_EQ(QueryText(db_, "SELECT doc_summary_config FROM namespaces WHERE ns_id='ns1'"),
              "{}");
}

// tenants has name / type / dedup_scope (+ default) and the denormalized
// cross_tenant_whitelist column.
TEST_F(CatalogSchemaTest, TenantsColumnsAndDedupScopeDefault) {
    auto cols = TableColumns(db_, "tenants");
    for (const char* c : {"name", "type", "dedup_scope", "cross_tenant_whitelist"}) {
        EXPECT_TRUE(Contains(cols, c)) << "tenants missing column: " << c;
    }
    ASSERT_EQ(sqlite3_exec(db_,
        "INSERT INTO tenants(tenant_id, created_at) VALUES('t9', 0)",
        nullptr, nullptr, nullptr), SQLITE_OK) << sqlite3_errmsg(db_);
    EXPECT_EQ(QueryText(db_, "SELECT dedup_scope FROM tenants WHERE tenant_id='t9'"),
              "tenant");
}

// content_refs reserves target_unit_id (Phase 2 cross-Unit ref, OPEN-3 UL-6).
TEST_F(CatalogSchemaTest, ContentRefsHasTargetUnitId) {
    auto cols = TableColumns(db_, "content_refs");
    EXPECT_TRUE(Contains(cols, "target_unit_id"));
}

// ns_acl uses the 6-column ARCH schema (not the early simplified 3-col one).
TEST_F(CatalogSchemaTest, NsAclSixColumnSchema) {
    auto cols = TableColumns(db_, "ns_acl");
    for (const char* c : {"ns_id", "grantee_tenant_id", "grantee_user_id",
                          "role", "granted_at", "granted_by_user_id"}) {
        EXPECT_TRUE(Contains(cols, c)) << "ns_acl missing column: " << c;
    }
}

// nodes seeded with exactly one 'local' row.
TEST_F(CatalogSchemaTest, NodesSeededWithLocal) {
    EXPECT_EQ(QueryInt(db_, "SELECT COUNT(*) FROM nodes"), 1);
    EXPECT_EQ(QueryText(db_, "SELECT node_id FROM nodes"), "local");
}

// foreign_keys pragma is ON: inserting a unit with an unknown tenant_id fails.
TEST_F(CatalogSchemaTest, ForeignKeysEnforced) {
    int rc = sqlite3_exec(db_,
        "INSERT INTO units(unit_id, tenant_id, created_at) VALUES('u','nope',0)",
        nullptr, nullptr, nullptr);
    EXPECT_NE(rc, SQLITE_OK);  // FK violation → constraint error
}

// schema_version records at the current version after Open().
TEST_F(CatalogSchemaTest, SchemaVersionRecordsCatalog) {
    EXPECT_EQ(QueryInt(db_,
        "SELECT version FROM schema_version WHERE feature='catalog'"),
        kCatalogSchemaVersion);
}

// Re-opening an already-migrated on-disk db is a no-op and preserves data.
TEST(CatalogSchemaReopenTest, ReopenIsIdempotent) {
    // Use a temp file (WAL needs a real file to exercise the on-disk path).
    std::string path = std::string(::testing::TempDir()) + "catalog_reopen.db";
    sqlite3* probe = nullptr;
    sqlite3_open(path.c_str(), &probe);
    sqlite3_exec(probe, "DROP TABLE IF EXISTS namespaces", nullptr, nullptr, nullptr);
    sqlite3_close(probe);
    std::remove(path.c_str());

    {
        CatalogDb c1;
        ASSERT_TRUE(c1.Open(path).ok());
        ASSERT_EQ(sqlite3_exec(c1.db(),
            "INSERT INTO tenants(tenant_id, created_at) VALUES('keep', 1)",
            nullptr, nullptr, nullptr), SQLITE_OK);
    }
    {
        CatalogDb c2;
        Status st = c2.Open(path);
        ASSERT_TRUE(st.ok()) << st.message();
        // Data from the first session survived; migration did not re-run/clobber.
        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(c2.db(),
            "SELECT COUNT(*) FROM tenants WHERE tenant_id='keep'", -1, &stmt, nullptr);
        ASSERT_NE(stmt, nullptr);
        ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
        EXPECT_EQ(sqlite3_column_int(stmt, 0), 1);
        sqlite3_finalize(stmt);
    }
    std::remove(path.c_str());
}

// ── CatalogDb::Open branch coverage ──────────────────────────────────────────

// Open a second time on an already-open handle → kInvalidArgument guard branch.
TEST(CatalogDbOpenTest, SecondOpenOnOpenHandleIsInvalidArgument) {
    CatalogDb c;
    ASSERT_TRUE(c.Open(":memory:").ok());
    Status s = c.Open(":memory:");  // still open → guarded
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kInvalidArgument);
}

// A path that sqlite cannot open (parent dir absent) → the open-failure branch.
TEST(CatalogDbOpenTest, OpenUnreachablePathIsInternal) {
    CatalogDb c;
    Status s = c.Open("/nonexistent-dir-xyz/catalog.db");
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kInternal);
    EXPECT_NE(s.message().find("catalog.db open failed"), std::string::npos);
}

// Close() on an already-closed CatalogDb (db_ == nullptr) is a no-op, and a
// fresh Open after Close succeeds — exercises the Close guard both ways.
TEST(CatalogDbOpenTest, CloseThenReopenSucceeds) {
    CatalogDb c;
    ASSERT_TRUE(c.Open(":memory:").ok());
    c.Close();
    c.Close();  // idempotent — db_ already null
    EXPECT_EQ(c.db(), nullptr);
    EXPECT_TRUE(c.Open(":memory:").ok());  // a new in-memory db
}

// A registered provider that fails its Migrate aborts the whole batch → the
// migrator error propagates out of Open and the db is Closed (db_ == nullptr).
namespace {
class FailingSchemaProvider : public ISchemaProvider {
public:
    std::string FeatureName() const override { return "FAIL_TEST"; }
    int CurrentVersion() const override { return 1; }
    Status Migrate(sqlite3*, int, int) override {
        return Status::Internal("forced migrate failure");
    }
};
}  // namespace

TEST(CatalogDbOpenTest, FailingExtraProviderAbortsOpen) {
    CatalogDb c;
    FailingSchemaProvider failing;
    Status s = c.Open(":memory:", {&failing});
    EXPECT_FALSE(s.ok());
    EXPECT_NE(s.message().find("forced migrate failure"), std::string::npos);
    EXPECT_EQ(c.db(), nullptr) << "a failed Open must leave the db closed";
}

// A nullptr extra provider is skipped (the migrator tolerates null entries) — the
// Open still succeeds with just the catalog base schema.
TEST(CatalogDbOpenTest, NullExtraProviderIsSkipped) {
    CatalogDb c;
    Status s = c.Open(":memory:", {nullptr});
    EXPECT_TRUE(s.ok()) << s.message();
    EXPECT_NE(c.db(), nullptr);
}

// ── CatalogSchemaProvider::Migrate failure branch ────────────────────────────────

// Running the catalog provider against a query-only db makes the CREATE TABLE exec
// fail ("readonly database") → the migration-failed Status branch (catalog_schema).
TEST(CatalogSchemaProviderMigrateTest, ExecFailureReturnsInternal) {
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(db, "PRAGMA query_only=ON", nullptr, nullptr, nullptr),
              SQLITE_OK);
    CatalogSchemaProvider provider;
    Status s = provider.Migrate(db, 0, provider.CurrentVersion());
    EXPECT_FALSE(s.ok());
    EXPECT_NE(s.message().find("catalog schema migration failed"), std::string::npos);
    sqlite3_close(db);
}

}  // namespace
}  // namespace cortrix::catalog
