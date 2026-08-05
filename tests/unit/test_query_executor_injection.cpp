#include <gtest/gtest.h>

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cortrix/import/import_error.h"
#include "cortrix/import/query_executor.h"

// S2 SQL-injection fuzzing (DB import / §7 / V3-E-01 root-cause). Hammers BOTH query
// modes with adversarial input and asserts the security gate holds:
//   1. custom-SQL mode: every classic injection family is rejected by the keyword /
//      separator / comment gate (CX_ERR_F16A_INVALID_SQL).
//   2. table+DSL mode: a value, no matter how malicious its text, can ONLY ever land
//      as a $N bind param — never interpolated into the SQL string. This is the
//      structural defense (parameterized query), so even a payload that slips past a
//      blocklist cannot change the query shape.
// Families covered: OWASP corpus, UNION-based, boolean-based blind, time-based blind,
// stacked queries, comment-based, metadata/schema probing.
namespace cortrix::import {
namespace {

using nlohmann::json;

// --- 1. custom-SQL mode: classic payloads must all be rejected ---

const std::vector<std::string>& InjectionCorpus() {
    static const std::vector<std::string> corpus = {
        // OWASP-style tautologies appended to a SELECT.
        "SELECT * FROM users WHERE name = '' OR '1'='1'; --",
        "SELECT * FROM users WHERE id = 1 OR 1=1; DROP TABLE users; --",
        // UNION-based (the exact V3-E-01 attack: exfiltrate pg_authid).
        "SELECT name FROM users UNION SELECT password FROM pg_authid",
        "SELECT name FROM users UNION ALL SELECT secret FROM vault",
        "SELECT a FROM t WHERE id=1 UNION SELECT table_name FROM information_schema.tables",
        // boolean-based blind.
        "SELECT * FROM users WHERE id = 1 AND (SELECT 1 FROM pg_authid LIMIT 1) = 1",
        // time-based blind.
        "SELECT * FROM users WHERE id = 1 AND pg_sleep(10)",
        "SELECT * FROM users WHERE id = 1; SELECT pg_sleep(10)",
        // stacked queries (statement separator).
        "SELECT 1; INSERT INTO audit VALUES ('x')",
        "SELECT 1; UPDATE users SET admin = true",
        // comment-based.
        "SELECT * FROM users -- WHERE tenant = 'me'",
        "SELECT /* bypass */ * FROM users",
        // metadata / schema probing.
        "SELECT * FROM information_schema.columns",
        "SELECT * FROM pg_catalog.pg_user",
        // write/DDL verbs disguised mid-query.
        "SELECT * FROM users; TRUNCATE audit",
        "SELECT * FROM (DELETE FROM users RETURNING *) sub",
    };
    return corpus;
}

TEST(QueryInjectionTest, EveryCustomSqlPayloadIsRejected) {
    QueryConstraints c;
    for (const std::string& payload : InjectionCorpus()) {
        Status s = ValidateSqlKeywords(payload, c);
        EXPECT_FALSE(s.ok()) << "ACCEPTED a malicious payload: " << payload;
        EXPECT_NE(s.message().find("CX_ERR_F16A_INVALID_SQL"), std::string::npos) << payload;
    }
}

TEST(QueryInjectionTest, CompileRejectsInjectionInCustomSqlMode) {
    QueryConstraints c;
    FilterDslConstraints fc;
    for (const std::string& payload : InjectionCorpus()) {
        QueryRequest req;
        req.sql = payload;
        auto r = CompileQueryRequest(req, c, fc);
        EXPECT_FALSE(r.ok()) << payload;
    }
}

// Specifically pin the V3-E-01 attack string.
TEST(QueryInjectionTest, V3E01UnionSelectPgAuthidBlocked) {
    QueryConstraints c;
    Status s = ValidateSqlKeywords("'1' UNION SELECT password FROM pg_authid--", c);
    EXPECT_FALSE(s.ok());
}

// --- 2. table+DSL mode: malicious VALUES can only become bind params ---

// The structural guarantee: whatever the (post-validation) value text, it must NOT
// appear inside the generated SQL string — it lives only in bind_params.
TEST(QueryInjectionTest, DslValuesAreAlwaysParameterizedNeverInterpolated) {
    FilterDslConstraints fc;
    const std::vector<std::string> evil_values = {
        "active'; DROP TABLE users; --",
        "' OR '1'='1",
        "x' UNION SELECT password FROM pg_authid--",
        "1); DELETE FROM audit; --",
        "'; pg_sleep(10); --",
    };
    for (const std::string& evil : evil_values) {
        json f = {{"status", {{"eq", evil}}}};
        auto r = BuildQueryFromFilterDsl("users", f, {"id"}, fc);
        ASSERT_TRUE(r.ok()) << "DSL with a string value should compile (value is bound): " << evil;
        const CompiledQuery& q = r.value();
        // The value text must NOT be in the SQL — it is a $1 bind.
        EXPECT_EQ(q.sql.find(evil), std::string::npos)
            << "value leaked into SQL text: " << q.sql;
        EXPECT_NE(q.sql.find("$1"), std::string::npos);
        ASSERT_EQ(q.bind_params.size(), 1u);
        EXPECT_EQ(q.bind_params[0].text, evil);  // the raw payload is safely a bound value
        // The generated SQL shape is always "SELECT "id" FROM "users" WHERE "status" = $1".
        EXPECT_NE(q.sql.find("\"status\" = $1"), std::string::npos);
        // No stray separators / comment markers got synthesized.
        EXPECT_EQ(q.sql.find("--"), std::string::npos);
        EXPECT_EQ(q.sql.find(";"), std::string::npos);
    }
}

// Malicious FIELD names (the identifier side, which is NOT parameterizable) must be
// rejected by the identifier pattern, never quoted-through.
TEST(QueryInjectionTest, DslMaliciousFieldNamesRejected) {
    FilterDslConstraints fc;
    const std::vector<std::string> evil_fields = {
        "id; DROP TABLE users",
        "id) OR (1=1",
        "id'--",
        "(SELECT 1)",
        "id FROM pg_authid",
    };
    for (const std::string& field : evil_fields) {
        json f = {{field, {{"eq", "x"}}}};
        EXPECT_FALSE(ValidateFilterDsl(f, fc).ok()) << "accepted evil field: " << field;
        EXPECT_FALSE(BuildQueryFromFilterDsl("users", f, {}, fc).ok()) << field;
    }
}

// Malicious operator strings are rejected by the operator allowlist.
TEST(QueryInjectionTest, DslMaliciousOperatorsRejected) {
    FilterDslConstraints fc;
    for (const char* op : {"; DROP", "eq; --", "exec", "sleep", "or"}) {
        json f = {{"status", {{op, "x"}}}};
        EXPECT_FALSE(ValidateFilterDsl(f, fc).ok()) << op;
    }
}

// 'in' with malicious array elements: each element is its own bind param.
TEST(QueryInjectionTest, DslInOperatorBindsEachElement) {
    FilterDslConstraints fc;
    json f = {{"user_id", {{"in", {"u1", "'; DROP TABLE x; --", "u3"}}}}};
    auto r = BuildQueryFromFilterDsl("users", f, {}, fc);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value().bind_params.size(), 3u);
    EXPECT_NE(r.value().sql.find("IN ($1, $2, $3)"), std::string::npos);
    EXPECT_EQ(r.value().sql.find("DROP"), std::string::npos);  // payload is bound, not in SQL
}

}  // namespace
}  // namespace cortrix::import
