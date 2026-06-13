#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#include "cortrix/agent_trace/agent_trace_writer.h"
#include "cortrix/observability/observability_context.h"

namespace cortrix::agent_trace {

/// One MCP tool invocation as seen by the session handler (§7.1). The real MCP
/// transport (mcp-server/) adapts its call payload to this; standalone tests drive
/// it directly. params is the raw JSON the client sent (the handler truncates it
/// to ≤2KB before writing, §3).
struct McpToolCall {
    std::string tool_name;
    std::string params;   ///< raw call params JSON
};

/// The result of an MCP tool invocation (§7.1). On failure, is_success=false +
/// error_code/error_message are set; the handler then writes status=failed and
/// keeps only the error in result_summary (§3).
struct McpToolResult {
    bool is_success = true;
    std::string summary;          ///< result summary (truncated to ≤512 on write)
    std::optional<std::string> error_code;
    std::string error_message;
    int duration_ms = 0;
};

/// What a client offered at connection time (topic 5): an optional custom session
/// id (validated; a bad one is replaced by a server-generated id) + optional
/// agent identity. The handler resolves session_id and seeds an
/// observability::McpSession from it.
struct McpClientCapability {
    std::optional<std::string> client_session_id;
    std::optional<std::string> agent_id;
    std::optional<std::string> namespace_id;
};

/// MCP session auto-capture (F13 §7.1, S3). Manages the per-session lifecycle and
/// double-writes every tool_call (incl. failures) to agent_trace via the injected
/// IAgentTraceWriter:
///   - OnConnectionEstablished: resolve session_id (server-generated UUID, or a
///     validated client id), register it, bump cortrix_mcp_active_sessions.
///   - OnToolCall: per-tool_call trace_id (C1), Phase-1 hard limit 10K (drop +
///     metric beyond it), write the agent_trace row.
///   - OnConnectionClosed: write a session_end row + call writer.OnSessionEnd,
///     drop the active-session gauge.
///   - CheckIdleSessions: deterministic idle sweep (topic 5 idle timeout) — sessions
///     idle beyond f13_mcp_idle_timeout_seconds get a session_timeout row and are
///     evicted. The real wall-clock watcher thread wiring is D3.5; tests call this
///     directly (mirrors CleanupScheduler.RunCleanupNow).
///
/// Standalone: no MCP transport / no timer thread; pure call-driven + injected
/// writer + injected clock/uuid seams. Thread-safe via one mutex over the session
/// map.
class McpSessionHandler {
public:
    using UuidGenerator = std::function<std::string()>;
    using ClockFn = std::function<int64_t()>;  ///< returns Unix ms

    /// Phase-1 hard limit: a single session past this many tool_calls drops new
    /// traces (topic 5).
    static constexpr int kHardLimitToolCalls = 10000;

    /// @param writer  the trace writer (borrowed via shared_ptr; double-write sink).
    /// @param idle_timeout_seconds  topic 5 idle window (IGlobalConfig.f13_mcp_idle_timeout_seconds).
    McpSessionHandler(std::shared_ptr<IAgentTraceWriter> writer,
                      int idle_timeout_seconds);

    /// Test/D3.5 ctor with injectable uuid + clock seams.
    McpSessionHandler(std::shared_ptr<IAgentTraceWriter> writer,
                      int idle_timeout_seconds,
                      UuidGenerator uuid_gen,
                      ClockFn clock);

    /// Resolve + register a session; returns the resolved session_id. A valid
    /// client_session_id is kept; an invalid one is replaced by a generated id
    /// (topic 5). Increments the active-session gauge.
    std::string OnConnectionEstablished(const McpClientCapability& cap);

    /// Record one tool_call. Returns false if dropped by the hard limit (topic 5);
    /// otherwise writes an agent_trace row (status from result) and returns true.
    bool OnToolCall(const std::string& session_id, const McpToolCall& call,
                    const McpToolResult& result);

    /// Write a session_end row + finalize; decrements the active-session gauge.
    void OnConnectionClosed(const std::string& session_id);

    /// Sweep registered sessions: any whose last activity is older than the idle
    /// window gets a session_timeout row (topic 5) and is evicted. Returns the count
    /// timed out. Deterministic (uses the injected clock); the wall-clock watcher
    /// thread is D3.5.
    int CheckIdleSessions();

    /// Number of currently-registered (open) sessions (test aid).
    size_t active_session_count() const;

    /// Truncate call params to ≤2KB keeping head + tail (§3 — params ≤2KB,
    /// preserve first 1.5KB + last 0.5KB + [...truncated...] marker). Exposed for tests.
    static std::string TruncateParams(const std::string& params);

    /// Truncate a result summary to ≤512 keeping head + tail (§3). Exposed for tests.
    static std::string TruncateResult(const std::string& summary);

    /// Format a failure into the result_summary (§3 — only error_code + first 256
    /// chars of the message are kept on failure). Exposed for tests.
    static std::string FormatError(const std::optional<std::string>& error_code,
                                   const std::string& error_message);

private:
    struct SessionState {
        std::optional<std::string> agent_id;
        std::optional<std::string> namespace_id;
        int64_t tool_call_count = 0;
        int64_t last_activity_ms = 0;
    };

    std::shared_ptr<IAgentTraceWriter> writer_;
    int idle_timeout_seconds_;
    UuidGenerator uuid_gen_;
    ClockFn clock_;

    mutable std::mutex mu_;
    std::unordered_map<std::string, SessionState> sessions_;
};

}  // namespace cortrix::agent_trace
