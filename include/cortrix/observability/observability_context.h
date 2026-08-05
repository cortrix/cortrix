#pragma once
#include <cstdint>
#include <map>
#include <optional>
#include <string>

#include "cortrix/common/result.h"
#include "cortrix/observability/trace_context.h"

namespace cortrix::observability {

enum class LogLevel { kDebug, kInfo, kWarn, kError };

const char* ToString(LogLevel level);

/// Raw HTTP headers as parsed by the server layer. A thin
/// case-sensitive name→value view; this layer reads X-Session-Id / X-Trace-Id /
/// X-Agent-Id from it. Defined here (not pulled from the http server) so the
/// shared ObservabilityContext stays dependency-light and unit-testable without
/// the server. The real middleware adapts the server's header type to this in
/// the D3.5 wiring.
struct HttpHeaders {
    std::map<std::string, std::string> values;

    /// Look up `name` (exact match); empty string when absent.
    std::string Get(const std::string& name) const {
        auto it = values.find(name);
        return it == values.end() ? std::string() : it->second;
    }
    bool Has(const std::string& name) const { return values.count(name) != 0; }
};

/// Minimal MCP session view needed to seed an ObservabilityContext.
/// The full McpSessionHandler lives in mcp_session.h; this is the read-only slice
/// the context factory consumes (the resolved session_id + optional agent_id),
/// kept here so the shared context header has no dependency on the MCP handler.
struct McpSession {
    std::string session_id;                  ///< server-resolved (generated or validated)
    std::optional<std::string> agent_id;     ///< from client capability, when supplied
    std::optional<std::string> namespace_id;
};

/// Thread-local observability scope (identity
/// extension). Carries the current TraceContext + the identity fields and
/// emits OBSERVABILITY_SPEC §6 structured logs with trace correlation. Each
/// thread has its own instance via ThreadLocal(); consumers (index, reranker, query, write,
/// fusion, CRAG + the whole trace chain) read/set the context and log through it.
///
/// This is the single identity-context source
/// shared by the ops-view track (agent_trace) and the user-view track
/// (operation_log). The trace members + the original 6 methods are FROZEN for
/// the namespace_pool / operation_log_emitter consumers (zero break);
/// this only ADDs the identity fields + From* factories below — fulfilling
/// (operation_log_emitter.cpp:49-62 + operation_logger.h:31-32 plan the context
/// to carry trace_id/session_id/user_id; the emitter notes it "does not yet"
/// carry user_id ⇒ planned). Real entry injection (middleware fills user_id from
/// the AuthContext) + the emitter reading these are wired later.
class ObservabilityContext {
public:
    static ObservabilityContext& ThreadLocal();

    // ===== Existing trace part (pool / operation log + trace-chain consumers; signatures frozen) =====
    const TraceContext* GetTraceContext() const;
    void SetTraceContext(TraceContext ctx);
    void ClearTraceContext();

    /// Build the OBS_SPEC §6 structured log line (JSON string) for `msg` at
    /// `level`, injecting the current trace_id/span_id (null when unset). Pure
    /// (no I/O) so callers and tests can inspect the exact payload.
    std::string FormatStructured(LogLevel level, const std::string& msg) const;

    /// Emit a structured log line. Phase 1 writes the FormatStructured() JSON to
    /// stderr; the OBS_SPEC §6 sink wiring (spdlog/OTel) lands with the full
    /// observability feature.
    void LogStructured(LogLevel level, const std::string& msg);

    // ===== Identity context (filled after HTTP/MCP entry parse) =====
    std::optional<std::string> session_id;   ///< VARCHAR(128); the operation log reads it for session correlation
    std::optional<std::string> agent_id;     ///< VARCHAR(128)
    std::optional<std::string> user_id;      ///< entry injects from the AuthContext (real inject wired later)
    std::optional<std::string> namespace_id;
    int64_t created_at = 0;                   ///< Unix ms the context was built (0 = unset)

    /// Build a context from parsed HTTP headers: validate
    /// X-Session-Id / X-Trace-Id / X-Agent-Id via ObservabilityValidator; an
    /// invalid header is dropped (left unset) — the caller emits the warning +
    /// metric. A missing/invalid X-Trace-Id leaves trace unset; the middleware
    /// (S2) is responsible for the server-generated fallback trace_id. created_at
    /// is stamped to now().
    static ObservabilityContext FromHttpHeaders(const HttpHeaders& headers);

    /// Build a context from an MCP session: the session_id is already
    /// server-resolved (generated or validated) by McpSessionHandler, so it is
    /// taken as-is; agent_id / namespace_id are copied through. created_at is
    /// stamped to now().
    static ObservabilityContext FromMcpCapability(const McpSession& session);

    /// Load this instance's identity + trace into the thread-local context. When
    /// work hops threads (async), the ctx must be carried + re-installed
    /// explicitly (C6) — there is no implicit propagation across threads.
    void SetThreadLocal() const;

private:
    std::optional<TraceContext> trace_;      ///< existing
};

/// Shared validator for the identity headers / MCP capability: a
/// single length + character-whitelist rule reused by both entry points. Per
/// CODING_CONVENTIONS §3 / F-FREEZE-1 it returns Result<T> + Status (NO
/// Result<T,E> double-param); an invalid value yields an InvalidArgument Status
/// carrying the CX_ERR_F13_INVALID_FILTER token, re-inflated to the full
/// Agent-friendly body at the API boundary.
class ObservabilityValidator {
public:
    /// Max accepted length for an identity field ("≤128").
    static constexpr int kMaxIdentityLength = 128;

    static Result<std::string> ValidateSessionId(const std::string& value);
    static Result<std::string> ValidateTraceId(const std::string& value);
    static Result<std::string> ValidateAgentId(const std::string& value);

    /// True iff `value` is non-empty, ≤ `max_length`, and every char is in the
    /// Whitelist `[a-zA-Z0-9_.:/-]`. Exposed for tests.
    static bool IsValidFormat(const std::string& value, int max_length);
};

}  // namespace cortrix::observability
