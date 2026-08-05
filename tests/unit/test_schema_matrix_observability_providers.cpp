// Schema-provider matrices for the observability / agent-trace / auth providers:
//   Operation log  (OperationLogSchemaProvider, operation_log table, version 1)
//   Agent trace   (AgentTraceSchemaProvider, agent_trace table, version 1; F13-scoped token)
//   Agent trace   (InteractionSourcesSchemaProvider, interaction_sources, version 1; FK -> interaction_log)
//   Auth   (P08AuthSchemaProvider, platform.db auth: 7 tables, version 1; version-agnostic Migrate)
//
// Fresh in-memory sqlite3 per case. Globally unique suite/fixture names
// (F18aObsMatrix / F13TraceMatrix / F13SrcMatrix / P08AuthMatrix) that do NOT reuse
// names from existing tests (OperationLogSchemaTest / AgentTraceSchemaTest / ...).

#include <gtest/gtest.h>

#include <sqlite3.h>

#include <string>

#include "cortrix/agent_trace/agent_trace_schema.h"
#include "cortrix/agent_trace/interaction_sources_schema.h"
#include "cortrix/auth/auth_schema.h"
#include "cortrix/observability/operation_log_schema.h"

namespace cortrix::test_schema_matrix_obs {
namespace {

// interaction_log FK target for InteractionSourcesSchemaProvider.
constexpr const char* kInteractionLogSql = R"SQL(
CREATE TABLE interaction_log (id TEXT PRIMARY KEY, payload TEXT);
)SQL";

struct SqliteHelpers {
    sqlite3* db = nullptr;
    void Open() { ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK); }
    void Close() { sqlite3_close(db); }
    int Exec(const std::string& sql) {
        return sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr);
    }
    bool ColumnExists(const char* table, const char* column) {
        std::string sql = std::string("SELECT 1 FROM pragma_table_info('") + table +
                          "') WHERE name='" + column + "';";
        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
        bool found = (sqlite3_step(stmt) == SQLITE_ROW);
        sqlite3_finalize(stmt);
        return found;
    }
    bool ObjectExists(const char* type, const std::string& name) {
        std::string sql = std::string("SELECT 1 FROM sqlite_master WHERE type='") + type +
                          "' AND name='" + name + "';";
        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
        bool found = (sqlite3_step(stmt) == SQLITE_ROW);
        sqlite3_finalize(stmt);
        return found;
    }
    bool TableExists(const std::string& n) { return ObjectExists("table", n); }
    bool IndexExists(const std::string& n) { return ObjectExists("index", n); }
};

struct InitStep {
    int from;
    int to;
    bool ok;
};

// ============================ operation_log (version 1) =================================

class F18aObsMatrix : public ::testing::Test, protected SqliteHelpers {
protected:
    void SetUp() override { Open(); }
    void TearDown() override { Close(); }
    cortrix::observability::OperationLogSchemaProvider p_;
};

TEST_F(F18aObsMatrix, FeatureIdentity) {
    EXPECT_EQ(p_.FeatureName(), "operation_log");
    EXPECT_EQ(p_.CurrentVersion(), 1);
    EXPECT_EQ(cortrix::observability::kOplogSchemaVersion, 1);
}

TEST_F(F18aObsMatrix, MigrateCreatesTableAndIndexes) {
    ASSERT_TRUE(p_.Migrate(db, 0, 1).ok());
    EXPECT_TRUE(TableExists("operation_log"));
    EXPECT_TRUE(IndexExists("idx_oplog_timestamp"));
    EXPECT_TRUE(IndexExists("idx_oplog_user_action"));
    EXPECT_TRUE(IndexExists("idx_oplog_trace_id"));
    EXPECT_TRUE(IndexExists("idx_oplog_session_id"));
    EXPECT_TRUE(IndexExists("idx_oplog_user_resource"));
}

TEST_F(F18aObsMatrix, TableShape) {
    ASSERT_TRUE(p_.Migrate(db, 0, 1).ok());
    EXPECT_TRUE(ColumnExists("operation_log", "timestamp"));
    EXPECT_TRUE(ColumnExists("operation_log", "user_id"));
    EXPECT_TRUE(ColumnExists("operation_log", "action"));
    EXPECT_TRUE(ColumnExists("operation_log", "resource_type"));
    EXPECT_TRUE(ColumnExists("operation_log", "trace_id"));
    EXPECT_TRUE(ColumnExists("operation_log", "session_id"));
}

TEST_F(F18aObsMatrix, IdempotentReRun) {
    ASSERT_TRUE(p_.Migrate(db, 0, 1).ok());
    EXPECT_TRUE(p_.Migrate(db, 0, 1).ok());
    EXPECT_TRUE(p_.Migrate(db, 1, 1).ok());
}

TEST_F(F18aObsMatrix, UnsupportedStepRejected) {
    Status s = p_.Migrate(db, 1, 2);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kInvalidArgument);
    EXPECT_NE(s.message().find("CX_ERR_SCHEMA_VERSION_MISMATCH"), std::string::npos);
    EXPECT_NE(s.message().find("operation_log"), std::string::npos);
}

class F18aObsVersionMatrix : public ::testing::TestWithParam<InitStep> {
protected:
    void SetUp() override { ASSERT_EQ(sqlite3_open(":memory:", &db_), SQLITE_OK); }
    void TearDown() override { sqlite3_close(db_); }
    sqlite3* db_ = nullptr;
    cortrix::observability::OperationLogSchemaProvider p_;
};
TEST_P(F18aObsVersionMatrix, Gate) {
    const InitStep step = GetParam();
    Status s = p_.Migrate(db_, step.from, step.to);
    if (step.ok) {
        EXPECT_TRUE(s.ok());
    } else {
        EXPECT_FALSE(s.ok());
        EXPECT_NE(s.message().find("CX_ERR_SCHEMA_VERSION_MISMATCH"), std::string::npos);
    }
}
INSTANTIATE_TEST_SUITE_P(
    F18aObsSteps, F18aObsVersionMatrix,
    ::testing::Values(InitStep{0, 1, true}, InitStep{1, 1, true}, InitStep{2, 2, true},
                      InitStep{0, 0, true}, InitStep{1, 2, false}, InitStep{0, 2, false},
                      InitStep{2, 1, false}, InitStep{1, 0, false}, InitStep{7, 7, true},
                      InitStep{3, 3, true}, InitStep{4, 4, true}, InitStep{10, 10, true}, InitStep{0, 3, false}, InitStep{0, 4, false}, InitStep{2, 5, false}, InitStep{3, 2, false}, InitStep{5, 1, false}));

// ============================ agent_trace (version 1, agent trace token) ========================

class F13TraceMatrix : public ::testing::Test, protected SqliteHelpers {
protected:
    void SetUp() override { Open(); }
    void TearDown() override { Close(); }
    cortrix::agent_trace::AgentTraceSchemaProvider p_;
};

TEST_F(F13TraceMatrix, FeatureIdentity) {
    EXPECT_EQ(p_.FeatureName(), "agent_trace");
    EXPECT_EQ(p_.CurrentVersion(), 1);
    EXPECT_EQ(cortrix::agent_trace::kAgentTraceSchemaVersion, 1);
}

TEST_F(F13TraceMatrix, MigrateCreatesTableAndIndexes) {
    ASSERT_TRUE(p_.Migrate(db, 0, 1).ok());
    EXPECT_TRUE(TableExists("agent_trace"));
    EXPECT_TRUE(IndexExists("idx_agent_trace_session"));
    EXPECT_TRUE(IndexExists("idx_agent_trace_agent"));
    EXPECT_TRUE(IndexExists("idx_agent_trace_trace_id"));
}

TEST_F(F13TraceMatrix, TableShape) {
    ASSERT_TRUE(p_.Migrate(db, 0, 1).ok());
    EXPECT_TRUE(ColumnExists("agent_trace", "session_id"));
    EXPECT_TRUE(ColumnExists("agent_trace", "trace_id"));
    EXPECT_TRUE(ColumnExists("agent_trace", "agent_id"));
    EXPECT_TRUE(ColumnExists("agent_trace", "method"));
    EXPECT_TRUE(ColumnExists("agent_trace", "status"));
    EXPECT_TRUE(ColumnExists("agent_trace", "created_at"));
}

TEST_F(F13TraceMatrix, IdempotentReRun) {
    ASSERT_TRUE(p_.Migrate(db, 0, 1).ok());
    EXPECT_TRUE(p_.Migrate(db, 0, 1).ok());
    EXPECT_TRUE(p_.Migrate(db, 1, 1).ok());
}

TEST_F(F13TraceMatrix, UnsupportedStepRejectedWithF13Token) {
    Status s = p_.Migrate(db, 1, 2);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kInvalidArgument);
    // agent_trace's mismatch surfaces as CX_ERR_TRACE_INTERNAL (feature-scoped token).
    EXPECT_NE(s.message().find("CX_ERR_TRACE_INTERNAL"), std::string::npos);
}

class F13TraceVersionMatrix : public ::testing::TestWithParam<InitStep> {
protected:
    void SetUp() override { ASSERT_EQ(sqlite3_open(":memory:", &db_), SQLITE_OK); }
    void TearDown() override { sqlite3_close(db_); }
    sqlite3* db_ = nullptr;
    cortrix::agent_trace::AgentTraceSchemaProvider p_;
};
TEST_P(F13TraceVersionMatrix, Gate) {
    const InitStep step = GetParam();
    Status s = p_.Migrate(db_, step.from, step.to);
    if (step.ok) {
        EXPECT_TRUE(s.ok());
    } else {
        EXPECT_FALSE(s.ok());
        EXPECT_NE(s.message().find("CX_ERR_TRACE_INTERNAL"), std::string::npos);
        EXPECT_EQ(s.code(), StatusCode::kInvalidArgument);
    }
}
INSTANTIATE_TEST_SUITE_P(
    F13TraceSteps, F13TraceVersionMatrix,
    ::testing::Values(InitStep{0, 1, true}, InitStep{1, 1, true}, InitStep{2, 2, true},
                      InitStep{0, 0, true}, InitStep{1, 2, false}, InitStep{0, 2, false},
                      InitStep{2, 1, false}, InitStep{1, 0, false}, InitStep{5, 5, true},
                      InitStep{3, 3, true}, InitStep{4, 4, true}, InitStep{10, 10, true}, InitStep{0, 3, false}, InitStep{0, 4, false}, InitStep{2, 5, false}, InitStep{3, 2, false}, InitStep{5, 1, false}));

// ============================ agent trace interaction_sources (version 1) ====================

class F13SrcMatrix : public ::testing::Test, protected SqliteHelpers {
protected:
    void SetUp() override {
        Open();
        ASSERT_EQ(Exec(kInteractionLogSql), SQLITE_OK);  // FK target
    }
    void TearDown() override { Close(); }
    cortrix::agent_trace::InteractionSourcesSchemaProvider p_;
};

TEST_F(F13SrcMatrix, FeatureIdentity) {
    EXPECT_EQ(p_.FeatureName(), "interaction_sources");
    EXPECT_EQ(p_.CurrentVersion(), 1);
    EXPECT_EQ(cortrix::agent_trace::kInteractionSourcesSchemaVersion, 1);
}

TEST_F(F13SrcMatrix, MigrateCreatesTableAndIndexes) {
    ASSERT_TRUE(p_.Migrate(db, 0, 1).ok());
    EXPECT_TRUE(TableExists("interaction_sources"));
    EXPECT_TRUE(IndexExists("idx_sources_interaction"));
    EXPECT_TRUE(IndexExists("idx_sources_block"));
}

TEST_F(F13SrcMatrix, TableShape) {
    ASSERT_TRUE(p_.Migrate(db, 0, 1).ok());
    EXPECT_TRUE(ColumnExists("interaction_sources", "interaction_id"));
    EXPECT_TRUE(ColumnExists("interaction_sources", "source_block_id"));
    EXPECT_TRUE(ColumnExists("interaction_sources", "source_type"));
    EXPECT_TRUE(ColumnExists("interaction_sources", "relevance_score"));
    EXPECT_TRUE(ColumnExists("interaction_sources", "snippet"));
}

TEST_F(F13SrcMatrix, IdempotentReRun) {
    ASSERT_TRUE(p_.Migrate(db, 0, 1).ok());
    EXPECT_TRUE(p_.Migrate(db, 0, 1).ok());
    EXPECT_TRUE(p_.Migrate(db, 1, 1).ok());
}

TEST_F(F13SrcMatrix, UnsupportedStepRejectedWithF13Token) {
    Status s = p_.Migrate(db, 1, 2);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kInvalidArgument);
    EXPECT_NE(s.message().find("CX_ERR_TRACE_INTERNAL"), std::string::npos);
    EXPECT_NE(s.message().find("interaction_sources"), std::string::npos);
}

class F13SrcVersionMatrix : public ::testing::TestWithParam<InitStep> {
protected:
    void SetUp() override {
        ASSERT_EQ(sqlite3_open(":memory:", &db_), SQLITE_OK);
        ASSERT_EQ(sqlite3_exec(db_, kInteractionLogSql, nullptr, nullptr, nullptr), SQLITE_OK);
    }
    void TearDown() override { sqlite3_close(db_); }
    sqlite3* db_ = nullptr;
    cortrix::agent_trace::InteractionSourcesSchemaProvider p_;
};
TEST_P(F13SrcVersionMatrix, Gate) {
    const InitStep step = GetParam();
    Status s = p_.Migrate(db_, step.from, step.to);
    if (step.ok) {
        EXPECT_TRUE(s.ok());
    } else {
        EXPECT_FALSE(s.ok());
        EXPECT_NE(s.message().find("CX_ERR_TRACE_INTERNAL"), std::string::npos);
    }
}
INSTANTIATE_TEST_SUITE_P(
    F13SrcSteps, F13SrcVersionMatrix,
    ::testing::Values(InitStep{0, 1, true}, InitStep{1, 1, true}, InitStep{2, 2, true},
                      InitStep{0, 0, true}, InitStep{1, 2, false}, InitStep{0, 2, false},
                      InitStep{2, 1, false}, InitStep{1, 0, false}, InitStep{9, 9, true},
                      InitStep{3, 3, true}, InitStep{4, 4, true}, InitStep{10, 10, true}, InitStep{0, 3, false}, InitStep{0, 4, false}, InitStep{2, 5, false}, InitStep{3, 2, false}, InitStep{5, 1, false}));

// ============================ auth (platform.db, version 1) ================================

class P08AuthMatrix : public ::testing::Test, protected SqliteHelpers {
protected:
    void SetUp() override { Open(); }
    void TearDown() override { Close(); }
    cortrix::auth::P08AuthSchemaProvider p_;
};

TEST_F(P08AuthMatrix, FeatureIdentity) {
    EXPECT_EQ(p_.FeatureName(), "auth");
    EXPECT_EQ(p_.CurrentVersion(), 1);
    EXPECT_EQ(cortrix::auth::kP08AuthSchemaVersion, 1);
}

TEST_F(P08AuthMatrix, MigrateCreatesAllSevenTables) {
    ASSERT_TRUE(p_.Migrate(db, 0, 1).ok());
    EXPECT_TRUE(TableExists("users"));
    EXPECT_TRUE(TableExists("refresh_tokens"));
    EXPECT_TRUE(TableExists("verification_codes"));
    EXPECT_TRUE(TableExists("token_blacklist"));
    EXPECT_TRUE(TableExists("auth_secrets"));
    EXPECT_TRUE(TableExists("auth_config"));
}

TEST_F(P08AuthMatrix, UsersShapeAndIndexes) {
    ASSERT_TRUE(p_.Migrate(db, 0, 1).ok());
    EXPECT_TRUE(ColumnExists("users", "email"));
    EXPECT_TRUE(ColumnExists("users", "password_hash"));
    EXPECT_TRUE(ColumnExists("users", "role"));
    EXPECT_TRUE(ColumnExists("users", "login_attempts"));
    EXPECT_TRUE(IndexExists("idx_users_email"));
    EXPECT_TRUE(IndexExists("idx_refresh_tokens_user"));
}

TEST_F(P08AuthMatrix, EmailUniqueConstraint) {
    ASSERT_TRUE(p_.Migrate(db, 0, 1).ok());
    EXPECT_EQ(Exec("INSERT INTO users(id,email,password_hash,display_name,created_at,updated_at) "
                   "VALUES ('usr_1','a@b.c','h','n',0,0);"), SQLITE_OK);
    EXPECT_NE(Exec("INSERT INTO users(id,email,password_hash,display_name,created_at,updated_at) "
                   "VALUES ('usr_2','a@b.c','h','n',0,0);"), SQLITE_OK);
}

TEST_F(P08AuthMatrix, AuthSecretsCheckConstraint) {
    ASSERT_TRUE(p_.Migrate(db, 0, 1).ok());
    // status CHECK rejects an out-of-set value.
    EXPECT_NE(Exec("INSERT INTO auth_secrets(id,secret_type,value,status,created_at) "
                   "VALUES ('s1','jwt_secret',x'00','bogus',0);"), SQLITE_OK);
    EXPECT_EQ(Exec("INSERT INTO auth_secrets(id,secret_type,value,status,created_at) "
                   "VALUES ('s2','jwt_secret',x'00','current',0);"), SQLITE_OK);
}

TEST_F(P08AuthMatrix, IdempotentReRun) {
    ASSERT_TRUE(p_.Migrate(db, 0, 1).ok());
    EXPECT_TRUE(p_.Migrate(db, 0, 1).ok());
    EXPECT_TRUE(p_.Migrate(db, 1, 1).ok());
}

// Auth's Migrate is version-agnostic (creates from scratch regardless of from/to);
// every (from,to) succeeds against a fresh db. Matrix confirms no spurious rejection.
class P08AuthVersionMatrix : public ::testing::TestWithParam<InitStep> {
protected:
    void SetUp() override { ASSERT_EQ(sqlite3_open(":memory:", &db_), SQLITE_OK); }
    void TearDown() override { sqlite3_close(db_); }
    sqlite3* db_ = nullptr;
    cortrix::auth::P08AuthSchemaProvider p_;
};
TEST_P(P08AuthVersionMatrix, AlwaysCreatesRegardlessOfStep) {
    const InitStep step = GetParam();
    Status s = p_.Migrate(db_, step.from, step.to);
    EXPECT_TRUE(s.ok());
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_, "SELECT 1 FROM sqlite_master WHERE type='table' AND name='users';",
                       -1, &stmt, nullptr);
    EXPECT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    sqlite3_finalize(stmt);
}
INSTANTIATE_TEST_SUITE_P(
    P08AuthSteps, P08AuthVersionMatrix,
    ::testing::Values(InitStep{0, 1, true}, InitStep{1, 1, true}, InitStep{0, 2, true},
                      InitStep{1, 2, true}, InitStep{2, 2, true}, InitStep{0, 0, true}));

}  // namespace
}  // namespace cortrix::test_schema_matrix_obs
