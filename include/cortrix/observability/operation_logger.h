#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "cortrix/common/result.h"
#include "cortrix/common/status.h"
#include "cortrix/observability/trace_context.h"

namespace cortrix::observability {

/// One row of the operation_log table (F18a §5.1). The CE user-facing record of
/// "what data operation did the user perform" — the business-semantics half of
/// the three-track split (F18a is the user-view track; F13 agent_trace is the
/// ops-view track; §2.13 System Observability is the SRE track).
///
/// Boundary note: this is the public struct. A downstream extension writes
/// its extra fields (ip / status / details_json / tenant_id) to a SEPARATE
/// audit_log_extension table keyed by operation_log.id — it does NOT add fields
/// here. So this struct is frozen for the CE contract.
struct OperationLogEntry {
    int64_t id = 0;                             ///< AUTOINCREMENT row id (set on read; 0 on write)
    int64_t timestamp = 0;                      ///< Unix ms (0 → logger fills now())
    std::string user_id;                        ///< real user_id / "anonymous" / "pg:<PG_user>:<pid>"
    std::string action;                         ///< topic 1 naming convention {resource}_{verb}, ≤32, lower, '_'
    std::optional<std::string> namespace_id;
    std::string resource_type;                  ///< document/namespace/memory/query/db_connection/db_import
    std::optional<std::string> resource_id;
    std::optional<std::string> summary;         ///< ≤100 chars; "[auto] " prefix for LLM-extracted
    std::optional<std::string> trace_id;        ///< topic 6 — from ObservabilityContext (NULL allowed)
    std::optional<std::string> session_id;      ///< C2 — from ObservabilityContext (NULL allowed)
};

/// Query filter for GET /api/v1/operations (F18a topic 2 D+, 8 dimensions). Defaults
/// match §6.1: limit 50 / cap 200, offset 0, DESC.
struct OperationLogFilter {
    std::optional<std::string> action;                       ///< single-value filter
    std::optional<std::vector<std::string>> action_in;       ///< topic 2 multi-select
    std::optional<std::string> namespace_id;
    std::optional<std::string> user_id;                      ///< topic 2 — admin cross-user
    std::optional<std::string> resource_type;
    std::optional<std::string> trace_id;                     ///< topic 6 + topic 2 link
    std::optional<int64_t> from_timestamp;                   ///< topic 2 time range (Unix ms)
    std::optional<int64_t> to_timestamp;
    int limit = 50;                                          ///< default 50 / cap 200 (§6.1)
    int offset = 0;
    std::string sort_order = "DESC";                         ///< "ASC" | "DESC"
};

/// Paginated query result (F18a §6.1 Response `meta`).
struct OperationLogQueryResult {
    std::vector<OperationLogEntry> entries;
    int64_t total_count = 0;
    bool has_next = false;
    int next_offset = 0;
};

/// Aggregate stats for Agent self-service monitoring (F18a topic 4).
struct OperationLogStats {
    int64_t total_count = 0;
    int64_t oldest_timestamp = 0;
    int64_t latest_timestamp = 0;
    int64_t size_bytes = 0;
};

/// Liveness/health snapshot for Agent self-service monitoring (F18a topic 4).
struct HealthStatus {
    bool is_healthy = true;
    std::string last_error;
    bool db_path_reachable = true;
    int64_t last_cleanup_timestamp = 0;
};

/// CE-public operation-logging contract (F18a §5.1, topic 4 B+). cortrix/ defines and
/// owns this interface; a downstream extended logger IS-A
/// IOperationLogger and double-writes operation_log + audit_log_extension — but
/// this tree has NO conditional compilation and does not know any extended logger
/// exists (GEN-OpenCore-Boundary). Consumers (Engine instrumentation / API handler / Cleanup
/// Scheduler) depend on this interface via DI, never on the concrete class.
///
/// Per CODING_CONVENTIONS §3, Query returns Result<T> (no Result<T,E>); a domain
/// error is carried as a Status whose message is prefixed with the CX_ERR_OPLOG_*
/// token (see operation_log_error.h OplogStatus), re-inflated to the full
/// Agent-friendly body at the API boundary.
class IOperationLogger {
public:
    virtual ~IOperationLogger() = default;

    /// Record one successful operation. Synchronous SQLite WAL write (topic 4: Log
    /// P95 < 1ms). `ctx` is the GEN-TraceContext W3C handle; default nullptr, the
    /// implementation prefers the entry's trace_id/session_id when set and falls
    /// back to `ctx`. Never throws across the Engine boundary (C4: exception
    /// isolation is the caller's instrumentation wrapper, but the logger itself is no-throw).
    virtual void Log(const OperationLogEntry& entry,
                     const TraceContext* ctx = nullptr) = 0;

    /// Batch insert, all-or-nothing in a single SQLite transaction (topic 4).
    virtual void BatchLog(const std::vector<OperationLogEntry>& entries,
                          const TraceContext* ctx = nullptr) = 0;

    /// Query with filter + pagination + permission scope (§6.1). Returns an error
    /// Status (CX_ERR_OPLOG_* token) on invalid filter / bad range / out-of-range
    /// pagination / unauthorized cross-user query.
    virtual Result<OperationLogQueryResult> Query(
        const OperationLogFilter& filter,
        const TraceContext* ctx = nullptr) = 0;

    /// Delete rows past retention / over the row cap (§8.3). Invoked by the
    /// CleanupScheduler under an advisory lock.
    virtual void Cleanup() = 0;

    /// Aggregate stats (topic 4 — Agent monitoring self-service).
    virtual OperationLogStats GetStats() = 0;

    /// Health snapshot (topic 4 — Agent monitoring self-service).
    virtual HealthStatus Health() = 0;
};

}  // namespace cortrix::observability
