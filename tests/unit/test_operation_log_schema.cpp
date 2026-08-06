#include <gtest/gtest.h>

#include <sqlite3.h>

#include <set>
#include <string>
#include <vector>

#include "cortrix/catalog/schema_provider.h"  // ISchemaProvider + SchemaMigrator
#include "cortrix/observability/operation_log_schema.h"

// S1 coverage: operation_log schema — provider identity, the table +
// 5 indices created via the real SchemaMigrator, the full column set, NULL-allowed
// trace/session, and idempotent re-migration. operation_log lives in the global
// DB and references no catalog table (no FK), so the operation log provider migrates
// standalone.
namespace cortrix::observability {
namespace {

// ---- small sqlite query helpers (test-local, mirror test_catalog_schema) ----

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

std::set<std::string> TableColumns(sqlite3* db, const std::string& table) {
    return QueryTextSet(db, "SELECT name FROM pragma_table_info('" + table + "')");
}

int64_t QueryInt(sqlite3* db, const std::string& sql) {
    sqlite3_stmt* stmt = nullptr;
    EXPECT_EQ(sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr), SQLITE_OK)
        << sqlite3_errmsg(db);
    int64_t v = -1;
    if (stmt && sqlite3_step(stmt) == SQLITE_ROW) v = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return v;
}

bool Contains(const std::set<std::string>& s, const std::string& k) {
    return s.find(k) != s.end();
}

// Fixture: in-memory global db, operation log provider migrated once via the real migrator.
class OperationLogSchemaTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_EQ(sqlite3_open(":memory:", &db_), SQLITE_OK);
        cortrix::catalog::SchemaMigrator m;
        m.Register(&provider_);
        Status st = m.MigrateCatalog(db_);  // global-db migration via shared engine
        ASSERT_TRUE(st.ok()) << st.message();
    }
    void TearDown() override {
        if (db_) sqlite3_close(db_);
    }
    OperationLogSchemaProvider provider_;
    sqlite3* db_ = nullptr;
};

TEST(OperationLogSchemaProviderTest, IdentityAndVersion) {
    OperationLogSchemaProvider p;
    EXPECT_EQ(p.FeatureName(), "operation_log");
    EXPECT_EQ(p.CurrentVersion(), 1);
    EXPECT_EQ(kOplogSchemaVersion, 1);
}

// Unsupported version step (e.g. Phase 2 1 → 2) is a version-mismatch error.
TEST(OperationLogSchemaProviderTest, UnexpectedVersionStepIsError) {
    OperationLogSchemaProvider p;
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    Status st = p.Migrate(db, 1, 2);
    EXPECT_FALSE(st.ok());
    EXPECT_NE(st.message().find("CX_ERR_SCHEMA_VERSION_MISMATCH"), std::string::npos);
    sqlite3_close(db);
}

// sqlite3_exec failure branch: a pre-existing index named "operation_log" defeats
// the schema DDL's "CREATE TABLE IF NOT EXISTS operation_log" (IF NOT EXISTS only
// tolerates same-type collisions), so Migrate returns CX_ERR_OPLOG_INTERNAL.
TEST(OperationLogSchemaProviderTest, ExecFailureReportsInternalError) {
    OperationLogSchemaProvider p;
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(db, "CREATE TABLE helper(x INTEGER)",
                           nullptr, nullptr, nullptr), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(db, "CREATE INDEX operation_log ON helper(x)",
                           nullptr, nullptr, nullptr), SQLITE_OK);
    Status st = p.Migrate(db, 0, kOplogSchemaVersion);
    EXPECT_FALSE(st.ok());
    EXPECT_NE(st.message().find("CX_ERR_OPLOG_INTERNAL"), std::string::npos);
    sqlite3_close(db);
}

// DoD: the operation_log table exists after migration, recorded at v1.
TEST_F(OperationLogSchemaTest, TableExistsAndVersionRecorded) {
    auto tables = QueryTextSet(db_, "SELECT name FROM sqlite_master WHERE type='table'");
    EXPECT_TRUE(Contains(tables, "operation_log"));
    cortrix::catalog::SchemaMigrator m;
    EXPECT_EQ(m.CurrentVersion(db_, "operation_log"), kOplogSchemaVersion);
}

// DoD: the full column set is present (10 columns).
TEST_F(OperationLogSchemaTest, AllColumnsPresent) {
    auto cols = TableColumns(db_, "operation_log");
    for (const char* c : {"id", "timestamp", "user_id", "action", "namespace_id",
                          "resource_type", "resource_id", "summary",
                          "trace_id", "session_id"}) {
        EXPECT_TRUE(Contains(cols, c)) << "operation_log missing column: " << c;
    }
    EXPECT_EQ(cols.size(), 10u);
}

// DoD: all 5 indices exist.
TEST_F(OperationLogSchemaTest, FiveIndicesExist) {
    auto idx = QueryTextSet(
        db_, "SELECT name FROM sqlite_master WHERE type='index' "
             "AND tbl_name='operation_log' AND name LIKE 'idx_oplog_%'");
    for (const char* i : {"idx_oplog_timestamp", "idx_oplog_user_action",
                          "idx_oplog_trace_id", "idx_oplog_session_id",
                          "idx_oplog_user_resource"}) {
        EXPECT_TRUE(Contains(idx, i)) << "missing index: " << i;
    }
    EXPECT_EQ(idx.size(), 5u);
}

// NOT NULL columns enforced: timestamp / user_id / action / resource_type are
// required; namespace_id / resource_id / summary / trace_id / session_id are
// nullable (Issue 6: NULL trace allowed for cron / internal cleanup).
TEST_F(OperationLogSchemaTest, NotNullAndNullableColumns) {
    // A minimal valid row: only the 4 NOT NULL columns supplied; the rest NULL.
    ASSERT_EQ(sqlite3_exec(db_,
        "INSERT INTO operation_log(timestamp, user_id, action, resource_type) "
        "VALUES(1747584000000, 'alice', 'memory_create', 'memory')",
        nullptr, nullptr, nullptr), SQLITE_OK) << sqlite3_errmsg(db_);
    EXPECT_EQ(QueryInt(db_,
        "SELECT COUNT(*) FROM operation_log WHERE namespace_id IS NULL "
        "AND resource_id IS NULL AND summary IS NULL "
        "AND trace_id IS NULL AND session_id IS NULL"), 1);
    // id AUTOINCREMENTed to a positive value.
    EXPECT_GT(QueryInt(db_, "SELECT id FROM operation_log WHERE user_id='alice'"), 0);

    // Omitting a NOT NULL column (action) is rejected.
    int rc = sqlite3_exec(db_,
        "INSERT INTO operation_log(timestamp, user_id, resource_type) "
        "VALUES(1, 'bob', 'query')",
        nullptr, nullptr, nullptr);
    EXPECT_NE(rc, SQLITE_OK) << "NOT NULL action constraint not enforced";
}

// A fully-populated row round-trips, including the pg:<PG_user>:<pid> user_id
// format (Issue 8) and the trace/session correlation columns (Issue 6 / C2).
TEST_F(OperationLogSchemaTest, FullRowRoundTripWithPgUserAndTrace) {
    ASSERT_EQ(sqlite3_exec(db_,
        "INSERT INTO operation_log"
        "(timestamp, user_id, action, namespace_id, resource_type, resource_id, "
        " summary, trace_id, session_id) VALUES"
        "(1747584000000, 'pg:datauser:4242', 'database_import', 'sales', "
        " 'db_import', 'task-7', '[auto] imported 3 tables', 'trace-001', 'sess-abc')",
        nullptr, nullptr, nullptr), SQLITE_OK) << sqlite3_errmsg(db_);

    sqlite3_stmt* stmt = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(db_,
        "SELECT user_id, action, namespace_id, trace_id, session_id "
        "FROM operation_log WHERE resource_id='task-7'", -1, &stmt, nullptr), SQLITE_OK);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    auto col = [&](int i) {
        const char* t = reinterpret_cast<const char*>(sqlite3_column_text(stmt, i));
        return std::string(t ? t : "");
    };
    EXPECT_EQ(col(0), "pg:datauser:4242");
    EXPECT_EQ(col(1), "database_import");
    EXPECT_EQ(col(2), "sales");
    EXPECT_EQ(col(3), "trace-001");
    EXPECT_EQ(col(4), "sess-abc");
    sqlite3_finalize(stmt);
}

// Re-running the migration on an already-migrated db is a no-op (version-gated by
// the migrator) and the defensive IF NOT EXISTS DDL also tolerates a direct re-run.
TEST_F(OperationLogSchemaTest, MigrationIsIdempotent) {
    // Insert a row, then re-run the migrator: data must survive, schema unchanged.
    ASSERT_EQ(sqlite3_exec(db_,
        "INSERT INTO operation_log(timestamp, user_id, action, resource_type) "
        "VALUES(1, 'keep', 'ns_create', 'namespace')",
        nullptr, nullptr, nullptr), SQLITE_OK) << sqlite3_errmsg(db_);

    cortrix::catalog::SchemaMigrator m2;
    m2.Register(&provider_);
    Status st = m2.MigrateCatalog(db_);
    ASSERT_TRUE(st.ok()) << st.message();
    EXPECT_EQ(QueryInt(db_, "SELECT COUNT(*) FROM operation_log WHERE user_id='keep'"), 1);

    // Direct re-exec of the DDL is also a no-op (IF NOT EXISTS).
    EXPECT_TRUE(provider_.Migrate(db_, 1, 1).ok());  // already-current call
    EXPECT_EQ(QueryInt(db_, "SELECT COUNT(*) FROM operation_log"), 1);
}

}  // namespace
}  // namespace cortrix::observability
