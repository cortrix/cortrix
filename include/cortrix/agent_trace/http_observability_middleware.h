#pragma once
#include <functional>
#include <string>
#include <vector>

#include "cortrix/observability/observability_context.h"

namespace cortrix::agent_trace {

/// Result of running the HTTP observability middleware over one request's
/// headers. The `context` is ready to install on the thread-local
/// (ctx.SetThreadLocal()); `warnings` lists the headers that were present but
/// invalid (the caller sets `X-Cortrix-Header-Warning: invalid-format` on the
/// response when non-empty); `generated_trace_id` is true when the middleware had
/// to synthesize a trace_id because X-Trace-Id was absent or invalid (topic 4 —
/// each tool_call gets its own trace_id).
struct HttpObservabilityResult {
    observability::ObservabilityContext context;
    std::vector<std::string> warnings;        ///< header names that failed validation
    bool generated_trace_id = false;
};

/// Stateless parser+validator for the three identity headers:
///   X-Session-Id / X-Trace-Id / X-Agent-Id
/// Pure (no server dependency) so it is unit-testable in isolation; the integration
/// wiring adapts httplib::Request headers → observability::HttpHeaders and calls
/// Process(), then installs the returned context on the thread-local and copies
/// the warnings onto the response. Invalid headers are dropped (topic 4 — ignore +
/// warning + metric, never reject the request); a missing/invalid X-Trace-Id is
/// replaced by a freshly generated trace_id.
class HttpObservabilityMiddleware {
public:
    /// Trace-id generator seam. Default = a v4-style UUID. Tests inject a
    /// deterministic generator. Must return a value that passes
    /// ObservabilityValidator (≤128 + whitelist) — the default does.
    using TraceIdGenerator = std::function<std::string()>;

    HttpObservabilityMiddleware();
    explicit HttpObservabilityMiddleware(TraceIdGenerator gen);

    /// Parse + validate `headers`, record cortrix_invalid_header_total for each
    /// invalid one, and return the ready-to-install context + warnings.
    HttpObservabilityResult Process(const observability::HttpHeaders& headers) const;

    /// The CORS `allowed_headers` list this layer contributes. Exposed so the
    /// server's CORS config (config/cors.yaml) and tests share one source.
    static const std::vector<std::string>& CorsAllowedHeaders();

    /// The CORS `allowed_methods` list (GET / POST / OPTIONS preflight).
    static const std::vector<std::string>& CorsAllowedMethods();

private:
    TraceIdGenerator gen_;
};

/// Generate a v4-style UUID (lowercase hex, 8-4-4-4-12). ASCII + within the
/// identity whitelist, so it round-trips through ObservabilityValidator. Exposed
/// for reuse (MCP session id generation) + tests.
std::string GenerateUuidV4();

}  // namespace cortrix::agent_trace
