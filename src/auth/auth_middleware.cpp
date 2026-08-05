#include "cortrix/auth/auth_middleware.h"
#include "cortrix/server/http_server.h"
#include "cortrix/logging/logging.h"
// [F13 S2 · D3.5 wiring] Populate the thread-local ObservabilityContext on every
// agent-facing route from the F13 identity headers + the authenticated user_id, so
// the downstream Engine instrumentation (agent_trace, §11) and the F18a
// operation_log emitter (operation_log_emitter.cpp:49-62) read a filled context.
// WithAuth is the single entry seam every route passes through; doing it here is the
// §5.1 C1/C2 "entry injects user_id from the P08 AuthContext" wiring.
#include "cortrix/agent_trace/http_observability_middleware.h"
#include "cortrix/observability/observability_context.h"

namespace cortrix {

namespace {

// Parse X-Session-Id/X-Trace-Id/X-Agent-Id via the shared middleware
// (validation + server-generated trace_id fallback, topic 4), overlay the
// authenticated user_id (§5.1 — the context's identity source for both the F13
// ops-view and the F18a user-view tracks), and install the result on the
// thread-local. `user_id` is empty on the auth-disabled dev path (the emitter then
// defaults to "anonymous", unchanged). Invalid headers are dropped + surfaced via
// X-Cortrix-Header-Warning (never reject the request). Pure-ADD: failure here
// cannot affect auth — it runs only after the auth decision and touches nothing but
// the thread-local context + a response warning header.
void InstallObservabilityContext(const httplib::Request& req, httplib::Response& res,
                                 const std::string& user_id, const std::string& agent_id) {
    observability::HttpHeaders headers;
    for (const char* name : {"X-Session-Id", "X-Trace-Id", "X-Agent-Id"}) {
        if (req.has_header(name)) headers.values[name] = req.get_header_value(name);
    }
    static const agent_trace::HttpObservabilityMiddleware kMiddleware;
    agent_trace::HttpObservabilityResult parsed = kMiddleware.Process(headers);
    // user_id is not a header — it comes from the authenticated principal (§5.1).
    if (!user_id.empty()) parsed.context.user_id = user_id;
    // agent_id: the X-Agent-Id header wins (§6.1); fall back to the auth principal's
    // agent_id only when the header supplied none.
    if (!parsed.context.agent_id.has_value() && !agent_id.empty()) {
        parsed.context.agent_id = agent_id;
    }
    parsed.context.SetThreadLocal();
    if (!parsed.warnings.empty()) {
        res.set_header("X-Cortrix-Header-Warning", "invalid-format");
    }
}

}  // namespace

httplib::Server::Handler WithAuth(ApiKeyAuth& auth, int required_permission, HttpHandler handler) {
    return
        [&auth, required_permission, handler](const httplib::Request& req, httplib::Response& res) {
            RequestContext rctx;
            rctx.request_id = GenerateRequestId();
            rctx.start_time = std::chrono::steady_clock::now();

            // If auth is disabled, behave like NoAuth (grant full permissions)
            if (!auth.enabled()) {
                rctx.auth.tenant_id = "default_tenant";
                rctx.auth.user_id = "anonymous";
                rctx.auth.permissions = kPermRead | kPermWrite | kPermAdmin;
                // Dev/no-auth: still install the obs context from headers (no
                // real authenticated user_id, so use the CE no-auth identity).
                InstallObservabilityContext(req, res, /*user_id=*/rctx.auth.user_id,
                                            /*agent_id=*/rctx.auth.agent_id);
                handler(req, res, rctx);
                return;
            }

            // Extract token from Authorization header or X-API-Key
            std::string auth_header = req.get_header_value("Authorization");
            std::string api_key = req.get_header_value("X-API-Key");

            std::string token;
            if (!auth_header.empty()) {
                const std::string prefix = "Bearer ";
                if (auth_header.size() > prefix.size() &&
                    auth_header.substr(0, prefix.size()) == prefix) {
                    token = auth_header.substr(prefix.size());
                }
            }
            if (token.empty()) {
                token = api_key;
            }
            if (token.empty()) {
                WriteJsonError(res,
                               Status::Unauthenticated("Missing Authorization header or X-API-Key"),
                               rctx.request_id);
                return;
            }

            // Authenticate
            AuthContext actx;
            Status s = auth.Authenticate(token, &actx);
            if (!s.ok()) {
                WriteJsonError(res, s, rctx.request_id);
                return;
            }

            // Extract namespace from URL path if present (e.g., /api/v1/namespaces/:name)
            std::string ns;
            auto it = req.path_params.find("name");
            if (it != req.path_params.end()) {
                ns = it->second;
            }

            // Authorize
            s = auth.Authorize(actx, ns, required_permission);
            if (!s.ok()) {
                WriteJsonError(res, s, rctx.request_id);
                return;
            }

            rctx.auth = actx;
            // Auth succeeded: install the obs context from the F13 identity
            // headers + the authenticated user_id (§5.1 C1/C2). Runs after the auth
            // decision so it cannot influence authn/authz (pure-ADD).
            InstallObservabilityContext(req, res, actx.user_id, actx.agent_id);
            handler(req, res, rctx);
        };
}

httplib::Server::Handler NoAuth(HttpHandler handler) {
    return [handler](const httplib::Request& req, httplib::Response& res) {
        RequestContext rctx;
        rctx.request_id = GenerateRequestId();
        rctx.start_time = std::chrono::steady_clock::now();
        // Default AuthContext with admin permissions for unauthenticated endpoints
        rctx.auth.tenant_id = "default_tenant";
        rctx.auth.user_id = "anonymous";
        rctx.auth.permissions = kPermRead | kPermWrite | kPermAdmin;
        handler(req, res, rctx);
    };
}

}  // namespace cortrix
