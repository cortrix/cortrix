#include <gtest/gtest.h>

#include <sqlite3.h>

#include <set>
#include <string>

#include "cortrix/agent_trace/agent_trace_schema.h"
#include "cortrix/catalog/schema_provider.h"  // ISchemaProvider + SchemaMigrator

// S1 coverage: agent_trace schema (agent trace, topic 3) — provider identity, the
// table + 3 indices created via the real SchemaMigrator, the full 12-column set,
// NULL-allowed session/trace/agent/namespace, default status, and idempotent
// re-migration. agent_trace lives in the global DB and references no catalog
// table (no FK), so the agent trace provider migrates standalone.
namespace cortrix::agent_trace {
namespace {

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

// Fixture: in-memory global db, agent trace provider migrated once via the real migrator.
class AgentTraceSchemaTest : public ::testing::Test {
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
    AgentTraceSchemaProvider provider_;
    sqlite3* db_ = nullptr;
};

TEST(AgentTraceSchemaProviderTest, IdentityAndVersion) {
    AgentTraceSchemaProvider p;
    EXPECT_EQ(p.FeatureName(), "agent_trace");
    EXPECT_EQ(p.CurrentVersion(), 1);
    EXPECT_EQ(kAgentTraceSchemaVersion, 1);
}

// Unsupported version step (e.g. Phase 2 1 → 2) is an error.
TEST(AgentTraceSchemaProviderTest, UnexpectedVersionStepIsError) {
    AgentTraceSchemaProvider p;
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    Status st = p.Migrate(db, 1, 2);
    EXPECT_FALSE(st.ok());
    EXPECT_NE(st.message().find("CX_ERR_TRACE_INTERNAL"), std::string::npos);
    sqlite3_close(db);
}

// Already-current (1 → 1) call is a defensive no-op.
TEST(AgentTraceSchemaProviderTest, AlreadyCurrentIsNoop) {
    AgentTraceSchemaProvider p;
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    EXPECT_TRUE(p.Migrate(db, 1, 1).ok());
    sqlite3_close(db);
}

// sqlite3_exec failure branch: a name collision between the table we create and a
// pre-existing object of a DIFFERENT kind defeats IF NOT EXISTS, so the DDL fails
// and Migrate returns the CX_ERR_TRACE_INTERNAL exec-failure path.
TEST(AgentTraceSchemaProviderTest, ExecFailureReportsInternalError) {
    AgentTraceSchemaProvider p;
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    // Pre-create an index named "agent_trace" backed by a helper table; the schema
    // DDL's "CREATE TABLE IF NOT EXISTS agent_trace" then collides with this index
    // name (IF NOT EXISTS only suppresses same-type collisions).
    ASSERT_EQ(sqlite3_exec(db, "CREATE TABLE helper(x INTEGER)",
                           nullptr, nullptr, nullptr), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(db, "CREATE INDEX agent_trace ON helper(x)",
                           nullptr, nullptr, nullptr), SQLITE_OK);
    Status st = p.Migrate(db, 0, kAgentTraceSchemaVersion);
    EXPECT_FALSE(st.ok());
    EXPECT_NE(st.message().find("CX_ERR_TRACE_INTERNAL"), std::string::npos);
    sqlite3_close(db);
}

// DoD: the agent_trace table exists after migration, recorded at v1.
TEST_F(AgentTraceSchemaTest, TableExistsAndVersionRecorded) {
    auto tables = QueryTextSet(db_, "SELECT name FROM sqlite_master WHERE type='table'");
    EXPECT_TRUE(Contains(tables, "agent_trace"));
    cortrix::catalog::SchemaMigrator m;
    EXPECT_EQ(m.CurrentVersion(db_, "agent_trace"), kAgentTraceSchemaVersion);
}

// DoD: the full §4.1 column set is present (13 columns).
TEST_F(AgentTraceSchemaTest, AllColumnsPresent) {
    auto cols = TableColumns(db_, "agent_trace");
    for (const char* c : {"id", "session_id", "trace_id", "agent_id", "method",
                          "params", "result_summary", "duration_ms", "source",
                          "status", "error_code", "namespace_id", "created_at"}) {
        EXPECT_TRUE(Contains(cols, c)) << "agent_trace missing column: " << c;
    }
    EXPECT_EQ(cols.size(), 13u);
}

// DoD: all 3 §4.1 indices exist.
TEST_F(AgentTraceSchemaTest, ThreeIndicesExist) {
    auto idx = QueryTextSet(
        db_, "SELECT name FROM sqlite_master WHERE type='index' "
             "AND tbl_name='agent_trace' AND name LIKE 'idx_agent_trace_%'");
    for (const char* i : {"idx_agent_trace_session", "idx_agent_trace_agent",
                          "idx_agent_trace_trace_id"}) {
        EXPECT_TRUE(Contains(idx, i)) << "missing index: " << i;
    }
    EXPECT_EQ(idx.size(), 3u);
}

// method is the only NOT NULL identity column; created_at is NOT NULL (topic 3
// supplies it explicitly). session_id/trace_id/agent_id/namespace_id are nullable
// (a global / anonymous call has no NS, MVP rows have no session).
TEST_F(AgentTraceSchemaTest, NotNullAndNullableColumns) {
    // A minimal row: method + created_at + status default; the rest NULL.
    ASSERT_EQ(sqlite3_exec(db_,
        "INSERT INTO agent_trace(method, created_at) VALUES('session_end', 1747584000000)",
        nullptr, nullptr, nullptr), SQLITE_OK) << sqlite3_errmsg(db_);
    EXPECT_EQ(QueryInt(db_,
        "SELECT COUNT(*) FROM agent_trace WHERE session_id IS NULL "
        "AND trace_id IS NULL AND agent_id IS NULL AND namespace_id IS NULL "
        "AND error_code IS NULL"), 1);
    // status defaulted to 'success'.
    EXPECT_EQ(QueryText(db_, "SELECT status FROM agent_trace WHERE method='session_end'"),
              "success");
    // id AUTOINCREMENTed to a positive value.
    EXPECT_GT(QueryInt(db_, "SELECT id FROM agent_trace WHERE method='session_end'"), 0);

    // Omitting NOT NULL method is rejected.
    int rc = sqlite3_exec(db_,
        "INSERT INTO agent_trace(created_at, source) VALUES(1, 'http')",
        nullptr, nullptr, nullptr);
    EXPECT_NE(rc, SQLITE_OK) << "NOT NULL method constraint not enforced";
}

// A fully-populated row round-trips, including the failure shape (status=failed +
// error_code) and the trace/session correlation columns (C1 / C2).
TEST_F(AgentTraceSchemaTest, FullRowRoundTripWithFailure) {
    ASSERT_EQ(sqlite3_exec(db_,
        "INSERT INTO agent_trace"
        "(session_id, trace_id, agent_id, method, params, result_summary, "
        " duration_ms, source, status, error_code, namespace_id, created_at) VALUES"
        "('sess-abc', 'trace-001', 'sales_bot_v2', 'cortrix_query', "
        " '{\"query\":\"x\",\"top_k\":10}', NULL, 145, 'mcp', 'failed', "
        " 'CX_ERR_NS_NOT_FOUND', 'sales', 1747584000000)",
        nullptr, nullptr, nullptr), SQLITE_OK) << sqlite3_errmsg(db_);

    sqlite3_stmt* stmt = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(db_,
        "SELECT session_id, trace_id, agent_id, method, source, status, error_code, "
        "namespace_id, duration_ms FROM agent_trace WHERE trace_id='trace-001'",
        -1, &stmt, nullptr), SQLITE_OK);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    auto col = [&](int i) {
        const char* t = reinterpret_cast<const char*>(sqlite3_column_text(stmt, i));
        return std::string(t ? t : "");
    };
    EXPECT_EQ(col(0), "sess-abc");
    EXPECT_EQ(col(1), "trace-001");
    EXPECT_EQ(col(2), "sales_bot_v2");
    EXPECT_EQ(col(3), "cortrix_query");
    EXPECT_EQ(col(4), "mcp");
    EXPECT_EQ(col(5), "failed");
    EXPECT_EQ(col(6), "CX_ERR_NS_NOT_FOUND");
    EXPECT_EQ(col(7), "sales");
    EXPECT_EQ(sqlite3_column_int(stmt, 8), 145);
    sqlite3_finalize(stmt);
}

// Re-running the migration on an already-migrated db is a no-op (version-gated by
// the migrator) and the defensive IF NOT EXISTS DDL also tolerates a direct re-run.
TEST_F(AgentTraceSchemaTest, MigrationIsIdempotent) {
    ASSERT_EQ(sqlite3_exec(db_,
        "INSERT INTO agent_trace(method, created_at) VALUES('keep', 1)",
        nullptr, nullptr, nullptr), SQLITE_OK) << sqlite3_errmsg(db_);

    cortrix::catalog::SchemaMigrator m2;
    m2.Register(&provider_);
    Status st = m2.MigrateCatalog(db_);
    ASSERT_TRUE(st.ok()) << st.message();
    EXPECT_EQ(QueryInt(db_, "SELECT COUNT(*) FROM agent_trace WHERE method='keep'"), 1);

    // Direct re-exec of the DDL is also a no-op (IF NOT EXISTS).
    EXPECT_TRUE(provider_.Migrate(db_, 1, 1).ok());
    EXPECT_EQ(QueryInt(db_, "SELECT COUNT(*) FROM agent_trace"), 1);
}

}  // namespace
}  // namespace cortrix::agent_trace
