#include <gtest/gtest.h>

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cortrix/import/import_error.h"
#include "cortrix/import/query_executor.h"

// S2 coverage: the 5 D2 security constraints (F16a §3.4) — SELECT whitelist, dual
// -mode discrimination, JSON DSL → parameterized SQL, length / row caps. The
// dedicated injection-fuzzing corpus is in test_query_executor_injection.cpp.
namespace cortrix::import {
namespace {

using nlohmann::json;

// --- SELECT keyword whitelist (constraint 2) ---

TEST(QueryExecutorSqlTest, AcceptsPlainSelect) {
    QueryConstraints c;
    EXPECT_TRUE(ValidateSqlKeywords("SELECT id, name FROM users WHERE status = 'x'", c).ok());
    EXPECT_TRUE(ValidateSqlKeywords(
                    "SELECT a.id, b.title FROM articles a JOIN authors b ON a.author_id = b.id", c)
                    .ok());
}

TEST(QueryExecutorSqlTest, RejectsWriteVerbs) {
    QueryConstraints c;
    for (const char* sql : {
             "INSERT INTO users VALUES (1)",
             "UPDATE users SET x = 1",
             "DELETE FROM users",
             "DROP TABLE users",
             "TRUNCATE users",
             "ALTER TABLE users ADD c INT",
             "CREATE TABLE t (id INT)",
             "GRANT ALL ON users TO x",
         }) {
        Status s = ValidateSqlKeywords(sql, c);
        EXPECT_FALSE(s.ok()) << sql;
        EXPECT_NE(s.message().find("CX_ERR_F16A_INVALID_SQL"), std::string::npos) << sql;
    }
}

TEST(QueryExecutorSqlTest, RejectsNonSelectLeadingToken) {
    QueryConstraints c;
    EXPECT_FALSE(ValidateSqlKeywords("WITH t AS (SELECT 1) SELECT * FROM t", c).ok());
    EXPECT_FALSE(ValidateSqlKeywords("EXPLAIN SELECT * FROM users", c).ok());
    EXPECT_FALSE(ValidateSqlKeywords("", c).ok());
}

TEST(QueryExecutorSqlTest, EnforcesMaxLength) {
    QueryConstraints c;
    c.max_sql_length = 32;
    EXPECT_FALSE(ValidateSqlKeywords("SELECT * FROM a_table_with_a_really_long_name_here", c).ok());
}

// --- JSON DSL validation (constraint, v1.0.2 resolution 8) ---

TEST(QueryExecutorDslTest, AcceptsWhitelistedOperators) {
    FilterDslConstraints fc;
    json f = {
        {"status", {{"eq", "active"}}},
        {"created_at", {{"gt", "2026-01-01"}}},
        {"user_id", {{"in", {"u1", "u2"}}}},
        {"score", {{"between", {1, 10}}}},
        {"deleted_at", {{"is_null", true}}},
        {"metadata.tier", {{"eq", "gold"}}},
    };
    EXPECT_TRUE(ValidateFilterDsl(f, fc).ok());
}

TEST(QueryExecutorDslTest, RejectsUnknownOperator) {
    FilterDslConstraints fc;
    json f = {{"status", {{"regex", ".*"}}}};
    Status s = ValidateFilterDsl(f, fc);
    EXPECT_FALSE(s.ok());
    EXPECT_NE(s.message().find("operator not allowed"), std::string::npos);
}

TEST(QueryExecutorDslTest, RejectsFieldNotInAllowlist) {
    FilterDslConstraints fc;
    fc.allowed_fields = {"status", "created_at"};
    json ok = {{"status", {{"eq", "active"}}}};
    EXPECT_TRUE(ValidateFilterDsl(ok, fc).ok());
    json bad = {{"password", {{"eq", "x"}}}};
    EXPECT_FALSE(ValidateFilterDsl(bad, fc).ok());
    // metadata.* is always allowed even with an allowlist.
    json meta = {{"metadata.anything", {{"eq", "x"}}}};
    EXPECT_TRUE(ValidateFilterDsl(meta, fc).ok());
}

TEST(QueryExecutorDslTest, RejectsNestedExpressionValues) {
    FilterDslConstraints fc;
    // scalar operators must not take an object/array value (injection hiding spot).
    EXPECT_FALSE(ValidateFilterDsl(json{{"x", {{"eq", json::object()}}}}, fc).ok());
    EXPECT_FALSE(ValidateFilterDsl(json{{"x", {{"eq", json::array({1, 2})}}}}, fc).ok());
    EXPECT_FALSE(ValidateFilterDsl(json{{"x", {{"in", "not-an-array"}}}}, fc).ok());
    EXPECT_FALSE(ValidateFilterDsl(json{{"x", {{"between", {1}}}}}, fc).ok());
}

TEST(QueryExecutorDslTest, RejectsMalformedFieldName) {
    FilterDslConstraints fc;
    EXPECT_FALSE(ValidateFilterDsl(json{{"id; DROP TABLE", {{"eq", 1}}}}, fc).ok());
    EXPECT_FALSE(ValidateFilterDsl(json{{"id OR 1=1", {{"eq", 1}}}}, fc).ok());
}

// --- DSL → parameterized SQL (V3-E-01 anti-injection: values are $N binds) ---

TEST(QueryExecutorBuildTest, BuildsParameterizedQuery) {
    FilterDslConstraints fc;
    json f = {
        {"status", {{"eq", "active"}}},
        {"created_at", {{"gt", "2026-01-01"}}},
        {"user_id", {{"in", {"u1", "u2"}}}},
        {"metadata.tier", {{"eq", "gold"}}},
    };
    auto r = BuildQueryFromFilterDsl("users", f, {"id", "name", "email"}, fc);
    ASSERT_TRUE(r.ok()) << r.status().message();
    const CompiledQuery& q = r.value();

    // values are bound, not interpolated.
    EXPECT_NE(q.sql.find("$1"), std::string::npos);
    EXPECT_EQ(q.sql.find("active"), std::string::npos);   // value NOT in the SQL text
    EXPECT_EQ(q.sql.find("gold"), std::string::npos);
    EXPECT_EQ(q.bind_params.size(), 5u);                  // active, 2026-01-01, u1, u2, gold
    // columns quoted; table quoted; metadata accessor present.
    EXPECT_NE(q.sql.find("\"users\""), std::string::npos);
    EXPECT_NE(q.sql.find("\"id\""), std::string::npos);
    EXPECT_NE(q.sql.find("(metadata->>'tier')"), std::string::npos);
}

TEST(QueryExecutorBuildTest, EmptyColumnsSelectsStar) {
    FilterDslConstraints fc;
    auto r = BuildQueryFromFilterDsl("users", json::object(), {}, fc);
    ASSERT_TRUE(r.ok());
    EXPECT_NE(r.value().sql.find("SELECT *"), std::string::npos);
    EXPECT_TRUE(r.value().bind_params.empty());
}

TEST(QueryExecutorBuildTest, RejectsBadTableOrColumnIdentifier) {
    FilterDslConstraints fc;
    EXPECT_FALSE(BuildQueryFromFilterDsl("users; DROP TABLE x", json::object(), {}, fc).ok());
    EXPECT_FALSE(BuildQueryFromFilterDsl("users", json::object(), {"id", "x); --"}, fc).ok());
}

TEST(QueryExecutorBuildTest, BetweenAndIsNullRenderCorrectly) {
    FilterDslConstraints fc;
    auto r = BuildQueryFromFilterDsl(
        "t", json{{"age", {{"between", {18, 65}}}}, {"deleted_at", {{"is_null", true}}}}, {}, fc);
    ASSERT_TRUE(r.ok());
    EXPECT_NE(r.value().sql.find("BETWEEN $1 AND $2"), std::string::npos);
    EXPECT_NE(r.value().sql.find("IS NULL"), std::string::npos);
    EXPECT_EQ(r.value().bind_params.size(), 2u);  // only the BETWEEN bounds bind
}

// --- dual-mode discrimination (§6.2 oneOf) ---

TEST(QueryExecutorCompileTest, ExactlyOneOfTableOrSql) {
    QueryConstraints c;
    FilterDslConstraints fc;
    QueryRequest neither;
    EXPECT_FALSE(CompileQueryRequest(neither, c, fc).ok());

    QueryRequest both;
    both.table = "users";
    both.sql = "SELECT 1";
    EXPECT_FALSE(CompileQueryRequest(both, c, fc).ok());

    QueryRequest table_mode;
    table_mode.table = "users";
    table_mode.filter = json{{"status", {{"eq", "active"}}}};
    EXPECT_TRUE(CompileQueryRequest(table_mode, c, fc).ok());

    QueryRequest sql_mode;
    sql_mode.sql = "SELECT id FROM users WHERE status = 'active'";
    EXPECT_TRUE(CompileQueryRequest(sql_mode, c, fc).ok());
}

// --- QueryExecutor: validates, then defers live PG to D3.5 ---

TEST(QueryExecutorTest, ExecuteValidatesBeforeDeferringToD35) {
    QueryExecutor exec;
    QueryConstraints c;
    // a malformed request fails on validation (INVALID_SQL), not on the PG seam.
    QueryRequest bad;  // neither table nor sql
    auto r = exec.Execute("dsn", bad, c);
    EXPECT_FALSE(r.ok());
    EXPECT_NE(r.status().message().find("CX_ERR_F16A_INVALID_SQL"), std::string::npos);

    // a well-formed request passes the gate and hits the D3.5 connection seam.
    QueryRequest good;
    good.table = "users";
    good.filter = json{{"status", {{"eq", "active"}}}};
    auto r2 = exec.Execute("dsn", good, c);
    EXPECT_FALSE(r2.ok());  // standalone: no live PG
    EXPECT_NE(r2.status().message().find("CX_ERR_F16A_CONNECTION_FAILED"), std::string::npos);
    EXPECT_NE(r2.status().message().find("D3.5"), std::string::npos);
}

}  // namespace
}  // namespace cortrix::import
