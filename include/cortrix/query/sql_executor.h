#pragma once
#include <string>
#include "cortrix/query/route_result.h"
#include "cortrix/query/query_response.h"

namespace cortrix {

/// Abstract interface for SQL execution in the query pipeline.
/// Design ref: ARCHITECTURE_DEEP_DESIGN.md Section 3.4.2 (Intent Routing)
///             text-to-sql-implementation.md (Phase 1 implementation plan)
///
/// MVP: SqlExecutorStub always returns kSkipped (graceful degradation).
/// Phase 1: Real implementation with LLM SQL generation + PG execution.
class SqlExecutor {
public:
    virtual ~SqlExecutor() = default;

    /// Execute Text-to-SQL query.
    /// @param query_text: natural language query
    /// @param namespace_name: target namespace (for schema lookup)
    /// @param timeout_us: route-level timeout (microseconds)
    /// @param sql_result: [out] populated with SQL results if successful
    /// @return RouteResult with status and items
    virtual RouteResult Execute(const std::string& query_text,
                                const std::string& namespace_name,
                                int64_t timeout_us,
                                SqlResult& sql_result) = 0;

    /// Whether SQL execution is available (LLM configured + DB connected)
    virtual bool IsAvailable() const = 0;
};

/// Stub implementation that always returns kSkipped.
/// Used in MVP where Text-to-SQL is not yet implemented.
/// This provides graceful degradation: the SQL route is simply skipped,
/// and the degradation manager correctly classifies this as L1 or L0.
class SqlExecutorStub : public SqlExecutor {
public:
    RouteResult Execute(const std::string& /*query_text*/,
                        const std::string& /*namespace_name*/,
                        int64_t /*timeout_us*/,
                        SqlResult& sql_result) override {
        sql_result.has_result = false;
        RouteResult result;
        result.route_name = "sql";
        result.status = RouteStatus::kSkipped;
        result.latency_us = 0;
        return result;
    }

    bool IsAvailable() const override { return false; }
};

}  // namespace cortrix
