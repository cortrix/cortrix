#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cortrix/common/result.h"
#include "cortrix/import/import_error.h"
#include "cortrix/import/import_types.h"

namespace cortrix::import {

/// The 5 security constraints. Read-only connection +
/// SELECT whitelist + 5-min timeout + 10M row cap + operation-log audit. Field defaults are
/// the config defaults.
struct QueryConstraints {
    int timeout_seconds = 300;          ///< 5-minute query timeout
    int64_t max_rows = 10'000'000;      ///< 10M row hard cap
    int max_sql_length = 10'000;        ///< custom-SQL max length (max_sql_length)

    /// Statement-level keyword allowlist (the only verbs a SELECT body may use).
    std::vector<std::string> allowed_keywords = {
        "SELECT", "FROM", "WHERE", "JOIN", "ORDER", "BY", "LIMIT",
        "ON", "AS", "AND", "OR", "IN", "NOT"};

    /// Denylist (v1.0.2 V3 ruling 8 + V3-E-01): write verbs + set operations (block
    /// UNION-SELECT injection) + metadata schemas + sensitive catalog tables (block
    /// probing / boolean-blind exfiltration) + time-based blind functions.
    std::vector<std::string> denied_keywords = {
        "INSERT", "UPDATE", "DELETE", "DROP", "EXEC", "EXECUTE", "CREATE", "ALTER",
        "TRUNCATE", "GRANT", "REVOKE", "COPY", "VACUUM", "MERGE", "CALL",
        "UNION", "INTERSECT", "EXCEPT",                       // set ops
        "INFORMATION_SCHEMA", "PG_CATALOG",                   // metadata schemas
        // sensitive system catalogs (credentials / roles): a nested
        // "(SELECT ... FROM pg_authid)" is a boolean-blind exfiltration vector even
        // inside a single read-only SELECT, so the table names are denied outright.
        "PG_AUTHID", "PG_SHADOW", "PG_ROLES", "PG_USER", "PG_USER_MAPPINGS",
        "PG_SLEEP"};                                          // time-based blind
};

/// JSON DSL filter constraints (v1.0.2 V3 ruling 8). 10-operator allowlist; the
/// field allowlist is computed per-table (table columns + metadata.* subkeys).
struct FilterDslConstraints {
    std::vector<std::string> allowed_operators = {
        "eq", "ne", "gt", "gte", "lt", "lte", "in", "between", "like", "is_null"};
    /// Allowed top-level field names (the table's own columns). A "metadata.<k>"
    /// field is always allowed (maps to a JSONB ->> accessor). Empty = allow any
    /// simple identifier column (used when the caller hasn't fetched the schema).
    std::vector<std::string> allowed_fields;
};

/// A bound parameter value for a parameterized query ($1, $2, ...). Phase-1 textual;
/// the libpq paramValues array is built from `text` at integration (PQexecParams).
struct BindParam {
    std::string text;
};

/// The result of compiling a filter DSL: a parameterized SQL string ("... WHERE x =
/// $1 ...") + the ordered bind values. The SQL NEVER interpolates user values — that
/// is the whole point (V3-E-01 anti-injection).
struct CompiledQuery {
    std::string sql;
    std::vector<BindParam> bind_params;
};

/// Validate a raw SELECT body against the keyword allow/deny lists + length cap
/// (constraint "SELECT whitelist"). Returns CX_ERR_IMPORT_INVALID_SQL on
/// any write verb / set op / metadata probe / non-SELECT leading token. Comment
/// markers and statement separators (`;` stacked queries) are rejected too.
Status ValidateSqlKeywords(const std::string& sql, const QueryConstraints& constraints);

/// Validate a JSON DSL filter against the operator + field allowlists.
/// Returns CX_ERR_IMPORT_INVALID_SQL on an unknown operator, a field
/// not in the allowlist, or a malformed shape.
Status ValidateFilterDsl(const nlohmann::json& filter_dsl,
                         const FilterDslConstraints& constraints);

/// Compile a table + JSON DSL filter into a parameterized SELECT
/// (build_query_from_filter_dsl). Validates first (ValidateFilterDsl); on success the
/// returned SQL uses only $N placeholders and `columns` are emitted as a quoted
/// identifier list (validated against the allowlist). `table` must be a bare
/// identifier (pattern ^[A-Za-z_][A-Za-z0-9_]*$).
Result<CompiledQuery> BuildQueryFromFilterDsl(const std::string& table,
                                              const nlohmann::json& filter_dsl,
                                              const std::vector<std::string>& columns,
                                              const FilterDslConstraints& constraints);

/// Validate the whole dual-mode QueryRequest (oneOf): exactly one of
/// {table, sql}; table mode runs ValidateFilterDsl + identifier checks; sql mode
/// runs ValidateSqlKeywords. Returns the compiled, parameterized query for table
/// mode; for custom-SQL mode the (validated) SQL is returned verbatim with no binds.
Result<CompiledQuery> CompileQueryRequest(const QueryRequest& req,
                                          const QueryConstraints& constraints,
                                          const FilterDslConstraints& dsl_constraints);

/// Connection seam (connect_readonly). The real libpq read-only connection
/// is integration; standalone mocks this. Owns nothing the QueryExecutor outlives.
class IConnectionManager;  // fwd (connection_manager.h)

/// Executes a validated query against an external PostgreSQL. The real
/// libpq PQexecParams path (read-only role, statement_timeout, paramValues) is
/// integration; cortrix/ owns this CE interface so the ImportManager seam is mockable.
class IQueryExecutor {
public:
    virtual ~IQueryExecutor() = default;

    /// Estimate row count (SELECT COUNT(*)) for the R5 pre-check + progress total.
    /// Returns CX_ERR_IMPORT_ROWS_LIMIT_EXCEEDED if the estimate exceeds max_rows.
    virtual Result<int64_t> EstimateRowCount(const std::string& dsn,
                                             const QueryRequest& req,
                                             const QueryConstraints& constraints) = 0;

    /// Run the (already-validated) query and return rows. Enforces timeout + row cap
    /// at the connection level (integration). The dsn is resolved by the ConnectionManager
    /// just before this call and never logged.
    virtual Result<std::vector<DbRow>> Execute(const std::string& dsn,
                                               const QueryRequest& req,
                                               const QueryConstraints& constraints) = 0;
};

/// Production QueryExecutor — validates every request (the 5 constraints) then
/// (integration) connects read-only + runs PQexecParams. In standalone the Execute/Estimate
/// bodies return CX_ERR_IMPORT_CONNECTION_FAILED ("real PG → integration") AFTER validation,
/// so the security gate is fully exercised without a live PG. The validation free
/// functions above are what the fuzzing suite hammers.
class QueryExecutor : public IQueryExecutor {
public:
    explicit QueryExecutor(FilterDslConstraints dsl_constraints = {});

    Result<int64_t> EstimateRowCount(const std::string& dsn, const QueryRequest& req,
                                     const QueryConstraints& constraints) override;
    Result<std::vector<DbRow>> Execute(const std::string& dsn, const QueryRequest& req,
                                       const QueryConstraints& constraints) override;

private:
    FilterDslConstraints dsl_constraints_;
};

}  // namespace cortrix::import
