#include <cstdint>
#include "cortrix/import/query_executor.h"

#include <algorithm>
#include <cctype>
#include <regex>
#include <set>

namespace cortrix::import {

namespace {

// Bare SQL identifier (table / column / metadata subkey). Anchored so the WHOLE
// token must match — no spaces, quotes, parens, semicolons, comment markers.
const std::regex& IdentifierPattern() {
    static const std::regex kId("^[A-Za-z_][A-Za-z0-9_]*$");
    return kId;
}

bool IsBareIdentifier(const std::string& s) {
    return std::regex_match(s, IdentifierPattern());
}

// A "metadata.<subkey>" field reference (both parts bare identifiers).
bool IsMetadataField(const std::string& s, std::string& subkey_out) {
    const std::string prefix = "metadata.";
    if (s.rfind(prefix, 0) != 0) return false;
    subkey_out = s.substr(prefix.size());
    return IsBareIdentifier(subkey_out);
}

std::string ToUpper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return s;
}

// Split a SQL string into uppercase word tokens (runs of [A-Za-z_]). Used only for
// keyword allow/deny screening — value literals never reach the DB via this path in
// table mode (they go through bind params); custom-SQL mode is keyword-gated here.
std::vector<std::string> WordTokens(const std::string& sql) {
    std::vector<std::string> tokens;
    std::string cur;
    for (char c : sql) {
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            cur.push_back(c);
        } else if (!cur.empty()) {
            tokens.push_back(ToUpper(cur));
            cur.clear();
        }
    }
    if (!cur.empty()) tokens.push_back(ToUpper(cur));
    return tokens;
}

// A double-quoted identifier column ("col"), used for the parameterized SELECT
// column list. We validate the inner identifier first, so quoting is safe.
std::string QuoteIdent(const std::string& ident) { return "\"" + ident + "\""; }

}  // namespace

Status ValidateSqlKeywords(const std::string& sql, const QueryConstraints& constraints) {
    if (sql.empty()) {
        return ImportStatus(ImportErrorCode::kInvalidSql, "empty SQL");
    }
    if (static_cast<int>(sql.size()) > constraints.max_sql_length) {
        return ImportStatus(ImportErrorCode::kInvalidSql,
                          "SQL exceeds max length " + std::to_string(constraints.max_sql_length));
    }
    // Reject comment markers + statement separators outright (stacked queries /
    // comment-based injection: "-- ", "/* */", ";", "#"). A legitimate single SELECT
    // body needs none of these.
    static const std::vector<std::string> kBannedSubstrings = {"--", "/*", "*/", ";", "#"};
    for (const std::string& bad : kBannedSubstrings) {
        if (sql.find(bad) != std::string::npos) {
            return ImportStatus(ImportErrorCode::kInvalidSql,
                              "SQL contains a forbidden sequence");
        }
    }

    std::vector<std::string> tokens = WordTokens(sql);
    if (tokens.empty() || tokens.front() != "SELECT") {
        // Leading statement must be SELECT (a WITH-CTE / anything else is rejected in
        // V1.0 — keeps the gate simple and conservative).
        return ImportStatus(ImportErrorCode::kInvalidSql, "only SELECT statements are allowed");
    }

    std::set<std::string> denied(constraints.denied_keywords.begin(),
                                 constraints.denied_keywords.end());
    for (const std::string& tok : tokens) {
        if (denied.count(tok)) {
            return ImportStatus(ImportErrorCode::kInvalidSql,
                              "SQL contains a forbidden keyword: " + tok);
        }
    }
    return Status::Ok();
}

Status ValidateFilterDsl(const nlohmann::json& filter_dsl,
                         const FilterDslConstraints& constraints) {
    if (filter_dsl.is_null()) return Status::Ok();  // no filter = unconstrained SELECT
    if (!filter_dsl.is_object()) {
        return ImportStatus(ImportErrorCode::kInvalidSql, "filter must be a JSON object");
    }

    std::set<std::string> ops(constraints.allowed_operators.begin(),
                              constraints.allowed_operators.end());
    std::set<std::string> fields(constraints.allowed_fields.begin(),
                                 constraints.allowed_fields.end());

    for (auto it = filter_dsl.begin(); it != filter_dsl.end(); ++it) {
        const std::string& field = it.key();
        std::string subkey;
        bool is_meta = IsMetadataField(field, subkey);
        if (!is_meta && !IsBareIdentifier(field)) {
            return ImportStatus(ImportErrorCode::kInvalidSql,
                              "filter field is not a valid identifier: " + field);
        }
        // Field allowlist: when allowed_fields is non-empty, a bare field must be in
        // it (metadata.* is always allowed — it maps to a JSONB accessor on the
        // metadata column, whose subkeys are open).
        if (!is_meta && !fields.empty() && !fields.count(field)) {
            return ImportStatus(ImportErrorCode::kInvalidSql,
                              "filter field is not in the allowlist: " + field);
        }

        const nlohmann::json& cond = it.value();
        if (!cond.is_object() || cond.empty()) {
            return ImportStatus(ImportErrorCode::kInvalidSql,
                              "filter condition must be a non-empty {op: value} object");
        }
        for (auto cit = cond.begin(); cit != cond.end(); ++cit) {
            const std::string& op = cit.key();
            if (!ops.count(op)) {
                return ImportStatus(ImportErrorCode::kInvalidSql,
                                  "filter operator not allowed: " + op);
            }
            // operator-specific value shape checks.
            const nlohmann::json& val = cit.value();
            if (op == "in") {
                if (!val.is_array() || val.empty()) {
                    return ImportStatus(ImportErrorCode::kInvalidSql,
                                      "'in' operator requires a non-empty array");
                }
            } else if (op == "between") {
                if (!val.is_array() || val.size() != 2) {
                    return ImportStatus(ImportErrorCode::kInvalidSql,
                                      "'between' operator requires a [lo, hi] array");
                }
            } else if (op == "is_null") {
                if (!val.is_boolean()) {
                    return ImportStatus(ImportErrorCode::kInvalidSql,
                                      "'is_null' operator requires a boolean");
                }
            } else {
                // eq/ne/gt/gte/lt/lte/like: a scalar (string/number/bool). Reject
                // objects/arrays (no nested expressions — that is how injection hides).
                if (val.is_object() || val.is_array()) {
                    return ImportStatus(ImportErrorCode::kInvalidSql,
                                      "operator '" + op + "' requires a scalar value");
                }
            }
        }
    }
    return Status::Ok();
}

namespace {

// Render a scalar JSON value to its bind text (the value goes to libpq as a
// parameter, never interpolated, so no escaping concerns — this is purely the
// textual form PQexecParams will bind).
std::string ScalarToBindText(const nlohmann::json& v) {
    if (v.is_string()) return v.get<std::string>();
    if (v.is_boolean()) return v.get<bool>() ? "true" : "false";
    return v.dump();  // numbers
}

// Map a DSL operator to its SQL operator for the parameterized clause.
const char* SqlOperator(const std::string& op) {
    if (op == "eq") return "=";
    if (op == "ne") return "<>";
    if (op == "gt") return ">";
    if (op == "gte") return ">=";
    if (op == "lt") return "<";
    if (op == "lte") return "<=";
    if (op == "like") return "LIKE";
    return "=";  // unreachable post-validation
}

// Build the left-hand-side column expression: a bare column → "col"; a metadata.<k>
// → (metadata->>'<k>'). The identifier(s) are already validated.
std::string LhsExpr(const std::string& field) {
    std::string subkey;
    if (IsMetadataField(field, subkey)) {
        return "(metadata->>'" + subkey + "')";
    }
    return QuoteIdent(field);
}

}  // namespace

Result<CompiledQuery> BuildQueryFromFilterDsl(const std::string& table,
                                              const nlohmann::json& filter_dsl,
                                              const std::vector<std::string>& columns,
                                              const FilterDslConstraints& constraints) {
    if (!IsBareIdentifier(table)) {
        return ImportStatus(ImportErrorCode::kInvalidSql,
                          "table is not a valid identifier: " + table);
    }
    Status v = ValidateFilterDsl(filter_dsl, constraints);
    if (!v.ok()) return v;

    // column list — each column must be a bare identifier; empty = SELECT *.
    std::string col_list = "*";
    if (!columns.empty()) {
        std::string joined;
        for (const std::string& c : columns) {
            if (!IsBareIdentifier(c)) {
                return ImportStatus(ImportErrorCode::kInvalidSql,
                                  "column is not a valid identifier: " + c);
            }
            if (!joined.empty()) joined += ", ";
            joined += QuoteIdent(c);
        }
        col_list = joined;
    }

    CompiledQuery out;
    std::string where;
    int param_idx = 1;
    auto add_param = [&](const nlohmann::json& v) {
        out.bind_params.push_back(BindParam{ScalarToBindText(v)});
        return "$" + std::to_string(param_idx++);
    };

    if (filter_dsl.is_object()) {
        for (auto it = filter_dsl.begin(); it != filter_dsl.end(); ++it) {
            const std::string lhs = LhsExpr(it.key());
            const nlohmann::json& cond = it.value();
            for (auto cit = cond.begin(); cit != cond.end(); ++cit) {
                const std::string& op = cit.key();
                const nlohmann::json& val = cit.value();
                std::string clause;
                if (op == "is_null") {
                    clause = lhs + (val.get<bool>() ? " IS NULL" : " IS NOT NULL");
                } else if (op == "in") {
                    std::string placeholders;
                    for (const auto& e : val) {
                        if (!placeholders.empty()) placeholders += ", ";
                        placeholders += add_param(e);
                    }
                    clause = lhs + " IN (" + placeholders + ")";
                } else if (op == "between") {
                    std::string lo = add_param(val[0]);
                    std::string hi = add_param(val[1]);
                    clause = lhs + " BETWEEN " + lo + " AND " + hi;
                } else {
                    clause = lhs + " " + SqlOperator(op) + " " + add_param(val);
                }
                if (!where.empty()) where += " AND ";
                where += clause;
            }
        }
    }

    out.sql = "SELECT " + col_list + " FROM " + QuoteIdent(table);
    if (!where.empty()) out.sql += " WHERE " + where;
    return out;
}

Result<CompiledQuery> CompileQueryRequest(const QueryRequest& req,
                                          const QueryConstraints& constraints,
                                          const FilterDslConstraints& dsl_constraints) {
    const bool has_table = req.table.has_value() && !req.table->empty();
    const bool has_sql = req.sql.has_value() && !req.sql->empty();
    if (has_table == has_sql) {
        return ImportStatus(ImportErrorCode::kInvalidSql,
                          "exactly one of {table, sql} must be provided");
    }
    if (has_table) {
        return BuildQueryFromFilterDsl(*req.table, req.filter, req.columns, dsl_constraints);
    }
    // custom-SQL mode: keyword gate only; no binds (the SQL is the user's own SELECT).
    Status v = ValidateSqlKeywords(*req.sql, constraints);
    if (!v.ok()) return v;
    CompiledQuery out;
    out.sql = *req.sql;
    return out;
}

// --- QueryExecutor: validates, then defers the live PG path to D3.5 ---

QueryExecutor::QueryExecutor(FilterDslConstraints dsl_constraints)
    : dsl_constraints_(std::move(dsl_constraints)) {}

Result<int64_t> QueryExecutor::EstimateRowCount(const std::string& /*dsn*/,
                                                const QueryRequest& req,
                                                const QueryConstraints& constraints) {
    // Always run the full security gate first (this is exercised by standalone tests).
    Result<CompiledQuery> compiled = CompileQueryRequest(req, constraints, dsl_constraints_);
    if (!compiled.ok()) return compiled.status();
    // Real SELECT COUNT(*) over the read-only connection → D3.5.
    return ImportStatus(ImportErrorCode::kConnectionFailed,
                      "live PostgreSQL COUNT(*) is wired in D3.5 (standalone validates only)");
}

Result<std::vector<DbRow>> QueryExecutor::Execute(const std::string& /*dsn*/,
                                                  const QueryRequest& req,
                                                  const QueryConstraints& constraints) {
    Result<CompiledQuery> compiled = CompileQueryRequest(req, constraints, dsl_constraints_);
    if (!compiled.ok()) return compiled.status();
    // Real read-only PQexecParams (timeout + row cap enforced at the connection) → D3.5.
    return ImportStatus(ImportErrorCode::kConnectionFailed,
                      "live PostgreSQL execution is wired in D3.5 (standalone validates only)");
}

}  // namespace cortrix::import
