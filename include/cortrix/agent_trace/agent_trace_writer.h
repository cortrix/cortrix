#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "cortrix/common/result.h"
#include "cortrix/common/status.h"
#include "cortrix/observability/trace_context.h"

namespace cortrix::agent_trace {

/// One row written to the agent_trace table. The CE ops-view
/// record of "what call did the Agent make, how long, why it failed" — the
/// protocol-detail half of the three-track split (operation_log is the
/// user-view track; System Observability is the SRE track). status defaults
/// to "success"; a failure sets status + error_code and (per) keeps only the
/// error in result_summary.
///
/// Boundary note: this is the public struct. A downstream extension writes
/// its extra fields (input_tokens / output_tokens / query_pattern_id) to a SEPARATE
/// agent_trace_extension table keyed by agent_trace.id — it does NOT add fields
/// here. So this struct is frozen for the CE contract.
struct AgentTraceEntry {
    std::optional<std::string> session_id;
    std::optional<std::string> trace_id;
    std::optional<std::string> agent_id;
    std::string method;                         ///< — VARCHAR(64), NOT NULL
    std::optional<std::string> params;          ///< ≤2KB JSON (caller truncates)
    std::optional<std::string> result_summary;  ///< ≤512 (caller truncates)
    int duration_ms = 0;
    std::string source;                         ///< "http" | "mcp"

    // topic 3
    std::string status = "success";             ///< success / failed / cancelled / session_timeout
    std::optional<std::string> error_code;
    std::optional<std::string> namespace_id;

    int64_t created_at = 0;                     ///< Unix ms (0 → writer fills now())
};

/// Query filter for GET /api/v1/traces/{session_id}. Defaults match
///: limit 50 / cap 200, offset 0. The session_id is the path param (passed
/// to Query separately); these are the additional query-string dimensions.
struct TraceFilter {
    std::optional<std::string> trace_id;
    std::optional<std::string> agent_id;
    std::optional<std::string> status;          ///< success / failed / cancelled / session_timeout
    std::optional<std::string> namespace_id;
    std::optional<int64_t> from_timestamp;      ///< Unix ms
    std::optional<int64_t> to_timestamp;
    int limit = 50;                             ///< default 50 / cap 200
    int offset = 0;
};

/// Paginated session view. traces are ordered by created_at
/// then id; the meta aggregates session_start/end + total_duration_ms.
struct TraceSession {
    std::string session_id;
    int trace_count = 0;                        ///< rows returned in this page
    std::vector<AgentTraceEntry> traces;
    int64_t session_start = 0;                  ///< MIN(created_at) across the whole session
    int64_t session_end = 0;                    ///< MAX(created_at) across the whole session
    int total_duration_ms = 0;                  ///< SUM(duration_ms) across the whole session
    int64_t total_count = 0;                    ///< total rows matching the filter (for pagination)
    bool has_next = false;
    int next_offset = 0;
};

/// CE-public agent-trace writer contract. cortrix/ defines and
/// owns this interface; a downstream extension writer IS-A
/// IAgentTraceWriter and double-writes agent_trace + agent_trace_extension — but
/// this tree has NO conditional compilation and does not know any extension writer
/// exists (GEN-OpenCore-Boundary). Consumers (Engine instrumentation / API handler /
/// MCP session handler / Cleanup Scheduler) depend on this interface via DI, never
/// on the concrete class.
///
/// Per the coding conventions / F-FREEZE-1, Query returns Result<T> (no Result<T,E>);
/// a domain error is carried as a Status whose message is prefixed with the
/// CX_ERR_TRACE_* token (see agent_trace_error.h AgentTraceStatus), re-inflated to the full
/// Agent-friendly body at the API boundary.
class IAgentTraceWriter {
public:
    virtual ~IAgentTraceWriter() = default;

    /// Record one call (all calls, including failures). Synchronous SQLite WAL
    /// write. `ctx` is the GEN-TraceContext W3C handle (C6, default nullptr); the
    /// implementation prefers the entry's trace_id when set and falls back to
    /// `ctx`. Never throws across the Engine boundary (C4: the logger itself is
    /// no-throw; exception isolation is the caller's instrumentation wrapper).
    virtual void Write(const AgentTraceEntry& entry,
                       const observability::TraceContext* ctx = nullptr) = 0;

    /// Query one session's traces with filter + pagination + permission scope
    ///. Returns an error Status (CX_ERR_TRACE_* token) on invalid filter /
    /// bad range / out-of-range pagination. The caller (handler) is responsible
    /// for the cross-user permission check (admin vs UNAUTHORIZED) before calling.
    virtual Result<TraceSession> Query(
        const std::string& session_id,
        const TraceFilter& filter,
        const observability::TraceContext* ctx = nullptr) = 0;

    /// MCP session end marker (topic 5 — Server actively calls on disconnect). The
    /// session_end row itself is written by the caller via Write(); this hook lets
    /// an implementation finalize any per-session state.
    virtual void OnSessionEnd(const std::string& session_id,
                              const observability::TraceContext* ctx = nullptr) = 0;

    /// Startup session_timeout backstop (topic 5 v1.0.2): find sessions with no
    /// session_end / session_timeout row whose last activity is older than
    /// `idle_threshold_seconds`, and write a method=session_timeout row for each.
    virtual void CheckAndMarkTimeoutSessions(
        int idle_threshold_seconds,
        const observability::TraceContext* ctx = nullptr) = 0;

    /// Delete rows past the retention window (agent_trace_retention_days).
    /// Invoked by the CleanupScheduler under an advisory lock (S10).
    virtual void Cleanup() = 0;
};

}  // namespace cortrix::agent_trace
