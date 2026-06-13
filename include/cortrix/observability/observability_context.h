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

/// Raw HTTP headers as parsed by the server layer (F13 §6.1). A thin
/// case-sensitive name→value view; F13 reads X-Session-Id / X-Trace-Id /
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

/// Minimal MCP session view F13 needs to seed an ObservabilityContext (F13 §7.1).
/// The full McpSessionHandler lives in mcp_session.h; this is the read-only slice
/// the context factory consumes (the resolved session_id + optional agent_id),
/// kept here so the shared context header has no dependency on the MCP handler.
struct McpSession {
    std::string session_id;                  ///< server-resolved (generated or validated)
    std::optional<std::string> agent_id;     ///< from client capability, when supplied
    std::optional<std::string> namespace_id;
};

/// Thread-local observability scope (scaffolding D2-pre-6; F13 §5.1 identity
/// extension). Carries the current TraceContext + the F13 identity fields and
/// emits OBSERVABILITY_SPEC §6 structured logs with trace correlation. Each
/// thread has its own instance via ThreadLocal(); consumers (F01/F02/F04/F25/
/// F36/F37 + the whole trace chain) read/set the context and log through it.
///
/// F13 §5.1 v1.0.1 (Derek "extend in place, A"): this is the single identity-context source
/// shared by the F13 ops-view track (agent_trace) and the F18a user-view track
/// (operation_log). The trace members + the original 6 methods are FROZEN for
/// the F05 namespace_pool / F18a operation_log_emitter consumers (zero break);
/// F13 only ADDs the identity fields + From* factories below — fulfilling C1/C2
/// (operation_log_emitter.cpp:49-62 + operation_logger.h:31-32 plan the context
/// to carry trace_id/session_id/user_id; the emitter notes it "does not yet"
/// carry user_id ⇒ planned). Real entry injection (middleware fills user_id from
/// the P08 AuthContext) + the emitter reading these = D3.5 wiring.
class ObservabilityContext {
public:
    static ObservabilityContext& ThreadLocal();

    // ===== Existing trace part (F05/F18a + trace-chain consumers; signatures frozen) =====
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

    // ===== F13 ADD: identity context (C1/C2; filled after HTTP/MCP entry parse) =====
    std::optional<std::string> session_id;   ///< C2 — VARCHAR(128); F18a reads it for session correlation
    std::optional<std::string> agent_id;     ///< VARCHAR(128)
    std::optional<std::string> user_id;      ///< entry injects from P08 AuthContext (real inject = D3.5)
    std::optional<std::string> namespace_id;
    int64_t created_at = 0;                   ///< Unix ms the context was built (0 = unset)

    /// Build a context from parsed HTTP headers (F13 §6.1): validate
    /// X-Session-Id / X-Trace-Id / X-Agent-Id via ObservabilityValidator; an
    /// invalid header is dropped (left unset) — the caller emits the warning +
    /// metric. A missing/invalid X-Trace-Id leaves trace unset; the middleware
    /// (S2) is responsible for the server-generated fallback trace_id. created_at
    /// is stamped to now().
    static ObservabilityContext FromHttpHeaders(const HttpHeaders& headers);

    /// Build a context from an MCP session (F13 §7.1): the session_id is already
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

/// Shared validator for the F13 identity headers / MCP capability (topic 4): a
/// single length + character-whitelist rule reused by both entry points. Per
/// CODING_CONVENTIONS §3 / F-FREEZE-1 it returns Result<T> + Status (NO
/// Result<T,E> double-param); an invalid value yields an InvalidArgument Status
/// carrying the CX_ERR_F13_INVALID_FILTER token, re-inflated to the full
/// Agent-friendly body at the API boundary.
class ObservabilityValidator {
public:
    /// Max accepted length for an identity field (F13 §6.1 "≤128").
    static constexpr int kMaxIdentityLength = 128;

    static Result<std::string> ValidateSessionId(const std::string& value);
    static Result<std::string> ValidateTraceId(const std::string& value);
    static Result<std::string> ValidateAgentId(const std::string& value);

    /// True iff `value` is non-empty, ≤ `max_length`, and every char is in the
    /// F13 §6.1 whitelist `[a-zA-Z0-9_.:/-]`. Exposed for tests.
    static bool IsValidFormat(const std::string& value, int max_length);
};

}  // namespace cortrix::observability
