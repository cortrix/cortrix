#include "cortrix/server/routes/observability_routes.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>
#include <functional>

#include <nlohmann/json.hpp>

#include "cortrix/agent_friendly/error.h"
#include "cortrix/agent_trace/agent_trace_error.h"
#include "cortrix/agent_trace/agent_trace_writer.h"        // TraceFilter / AgentTraceEntry / TraceSession
#include "cortrix/agent_trace/agent_trace_writer_impl.h"   // AgentTraceWriterImpl (per-NS writer)
#include "cortrix/agent_trace/http_observability_middleware.h"
#include "cortrix/agent_trace/interactions_handler.h"
#include "cortrix/agent_trace/traces_handler.h"
#include "cortrix/auth/auth_middleware.h"
#include "cortrix/common/i_global_config.h"
#include "cortrix/memory/memory_store.h"                   // MemoryStore::db_handle (per-NS memory.db)
#include "cortrix/observability/observability_context.h"
#include "cortrix/resource/namespace_facade.h"             // per-request NS façade
#include "cortrix/resource/namespace_pool.h"
#include "cortrix/server/http_server.h"                    // WriteJsonResponse / WriteJsonError

namespace cortrix {

namespace {

using agent_trace::F13ErrorCode;
using agent_trace::InteractionListFilter;
using agent_trace::InteractionListView;
using agent_trace::InteractionSourcesView;
using agent_trace::RequesterContext;
using agent_trace::TraceFilter;
using agent_trace::TraceSession;
using json = nlohmann::json;

// ---- requester context -----------------------------------------------------

// The handlers' permission decisions key off (requester_user_id, is_admin). Both
// come from the authenticated AuthContext that WithAuth filled (the real P08
// identity). user_id may be empty when auth is disabled (dev) — the handlers treat
// that as a normal non-admin/admin requester unchanged.
RequesterContext MakeRequesterContext(const RequestContext& rctx) {
    return RequesterContext{rctx.auth.user_id, rctx.auth.is_admin()};
}

// ---- F13 identity headers (§6.1) -------------------------------------------

// Parse X-Session-Id/X-Trace-Id/X-Agent-Id via the shared middleware, install the
// resulting ObservabilityContext on the thread-local (so the downstream read path
// + any instrumentation see it), and surface the per-header warning (topic 4 —
// invalid headers are ignored, never reject the request). Pure adaptation
// httplib -> middleware.
void ParseAndInstallObservabilityHeaders(const httplib::Request& req,
                                         httplib::Response& res) {
    observability::HttpHeaders headers;
    for (const char* name : {"X-Session-Id", "X-Trace-Id", "X-Agent-Id"}) {
        if (req.has_header(name)) headers.values[name] = req.get_header_value(name);
    }
    static const agent_trace::HttpObservabilityMiddleware kMiddleware;
    agent_trace::HttpObservabilityResult parsed = kMiddleware.Process(headers);
    parsed.context.SetThreadLocal();
    if (!parsed.warnings.empty()) {
        res.set_header("X-Cortrix-Header-Warning", "invalid-format");
    }
}

// ---- error rendering (GEN-Agent CX_ERR_F13_* body, §9.1) -------------------

// Recover the "CX_ERR_F13_*" token from a Status message ("CX_ERR_F13_X: detail").
// "" if the message does not start with the token. Mirrors batch_submit_service's
// ExtractCxCode (the handlers encode the F13 identity in the message prefix).
std::string ExtractCxCode(const std::string& message) {
    if (message.rfind("CX_ERR_F13_", 0) != 0) return "";
    const size_t end = message.find_first_of(": \t", 0);
    return end == std::string::npos ? message : message.substr(0, end);
}

// Map a recovered "CX_ERR_F13_*" token back to its F13ErrorCode. The handlers only
// ever emit these four on the read path; anything unrecognized is treated as the
// internal fault (defensive — keeps the body well-formed).
F13ErrorCode F13CodeFromToken(const std::string& cx) {
    if (cx == "CX_ERR_F13_SESSION_NOT_FOUND")     return F13ErrorCode::kSessionNotFound;
    if (cx == "CX_ERR_F13_INVALID_FILTER")        return F13ErrorCode::kInvalidFilter;
    if (cx == "CX_ERR_F13_INTERACTION_NOT_FOUND") return F13ErrorCode::kInteractionNotFound;
    if (cx == "CX_ERR_F13_UNAUTHORIZED")          return F13ErrorCode::kUnauthorized;
    return F13ErrorCode::kInternal;
}

// Build the §9.2-required structured_data for `code` from the values this route
// holds. The structured_data contract is the call site's responsibility (see
// agent_trace_error.h); the route is where the concrete ids live (session_id /
// interaction_id from the path, required_role for the auth denial, error_id for an
// internal fault). `id_value` carries the session_id or interaction_id as relevant.
json BuildF13StructuredData(F13ErrorCode code, const std::string& id_value,
                            const std::string& request_id) {
    json sd = json::object();
    switch (code) {
        case F13ErrorCode::kSessionNotFound:
            sd["session_id"] = id_value;
            break;
        case F13ErrorCode::kInteractionNotFound:
            sd["interaction_id"] = id_value;
            break;
        case F13ErrorCode::kUnauthorized:
            sd["required_role"] = "admin";
            break;
        case F13ErrorCode::kInvalidFilter:
            // The handler-originated INVALID_FILTER carries no field breakdown; name
            // the filter generically (route-originated filter errors use the richer
            // WriteF13InvalidFilter path with the exact offending param).
            sd["invalid_field"] = "filter";
            sd["reason"] = "invalid filter value";
            break;
        case F13ErrorCode::kInternal:
        default:
            sd["error_id"] = request_id;
            break;
    }
    return sd;
}

// Render a handler Status (carrying a CX_ERR_F13_* token) as the GEN-Agent error
// body + the mapped HTTP status. `id_value` = the session_id / interaction_id the
// route was operating on (used to fill structured_data for the NOT_FOUND codes).
void WriteF13Error(httplib::Response& res, const Status& status,
                   const std::string& request_id, const std::string& id_value) {
    const std::string cx = ExtractCxCode(status.message());
    if (cx.empty()) {
        // Not an F13-tagged error (shouldn't happen from these handlers) — fall back
        // to the standard envelope so the response is still well-formed.
        WriteJsonError(res, status, request_id);
        return;
    }
    const F13ErrorCode code = F13CodeFromToken(cx);
    json sd = BuildF13StructuredData(code, id_value, request_id);
    json body;
    body["error"] = agent_friendly::ToJson(
        agent_trace::MakeF13Error(code, std::move(sd), status.message()));
    if (!request_id.empty()) {
        body["error"]["request_id"] = request_id;
        res.set_header("X-Request-Id", request_id);
    }
    res.set_content(body.dump(), "application/json");
    // The incoming Status already carries the StatusCode F13Status mapped from the
    // code (NotFound/PermissionDenied/InvalidArgument/Internal) -> reuse its
    // http_status() so the HTTP mapping is single-sourced.
    res.status = status.http_status();
}

// Render a route-originated CX_ERR_F13_INVALID_FILTER (a malformed query param) with
// the exact offending field + reason + a PII-guarded preview (§9.2 — value_preview
// optional, first 100 chars). HTTP 400.
void WriteF13InvalidFilter(httplib::Response& res, const std::string& request_id,
                           const std::string& field, const std::string& reason,
                           const std::string& value) {
    json sd;
    sd["invalid_field"] = field;
    sd["reason"] = reason;
    sd["value_preview"] = value.substr(0, 100);  // PII guard — first 100 chars only
    json body;
    body["error"] = agent_friendly::ToJson(agent_trace::MakeF13Error(
        F13ErrorCode::kInvalidFilter, std::move(sd),
        "CX_ERR_F13_INVALID_FILTER: " + field + " " + reason));
    if (!request_id.empty()) {
        body["error"]["request_id"] = request_id;
        res.set_header("X-Request-Id", request_id);
    }
    res.set_content(body.dump(), "application/json");
    res.status = 400;
}

// ---- query-param parsing ---------------------------------------------------

// Parse a non-negative integer query param. Returns true on success (value in
// *out); false (with *bad_field/*bad_reason set) when present but non-numeric.
// Absent param -> true and *out unchanged (caller's default kept).
bool ParseIntParam(const httplib::Request& req, const char* name, int* out,
                   std::string* bad_field, std::string* bad_reason,
                   std::string* bad_value) {
    if (!req.has_param(name)) return true;
    const std::string raw = req.get_param_value(name);
    try {
        size_t pos = 0;
        const long v = std::stol(raw, &pos);
        if (pos != raw.size()) throw std::invalid_argument("trailing");
        *out = static_cast<int>(v);
        return true;
    } catch (...) {
        *bad_field = name;
        *bad_reason = "must be an integer";
        *bad_value = raw;
        return false;
    }
}

// Parse a Unix-ms timestamp query param into an optional<int64_t>. Same contract as
// ParseIntParam; absent -> leaves *out as nullopt.
bool ParseTimestampParam(const httplib::Request& req, const char* name,
                         std::optional<int64_t>* out, std::string* bad_field,
                         std::string* bad_reason, std::string* bad_value) {
    if (!req.has_param(name)) return true;
    const std::string raw = req.get_param_value(name);
    try {
        size_t pos = 0;
        const long long v = std::stoll(raw, &pos);
        if (pos != raw.size()) throw std::invalid_argument("trailing");
        *out = static_cast<int64_t>(v);
        return true;
    } catch (...) {
        *bad_field = name;
        *bad_reason = "must be a Unix-ms integer";
        *bad_value = raw;
        return false;
    }
}

// ---- response serializers --------------------------------------------------

// One agent_trace row as the §8.1 JSON object. Optional columns become JSON null
// when unset (stable schema, GEN-Agent #7). created_at is the raw stored value
// (the writer stamps Unix ms; §8.1's example ISO string is illustrative).
json TraceEntryToJson(const agent_trace::AgentTraceEntry& e) {
    json j;
    j["session_id"]     = e.session_id     ? json(*e.session_id)     : json(nullptr);
    j["trace_id"]       = e.trace_id       ? json(*e.trace_id)       : json(nullptr);
    j["agent_id"]       = e.agent_id       ? json(*e.agent_id)       : json(nullptr);
    j["method"]         = e.method;
    j["params"]         = e.params         ? json(*e.params)         : json(nullptr);
    j["result_summary"] = e.result_summary ? json(*e.result_summary) : json(nullptr);
    j["duration_ms"]    = e.duration_ms;
    j["source"]         = e.source;
    j["status"]         = e.status;
    j["error_code"]     = e.error_code     ? json(*e.error_code)     : json(nullptr);
    j["namespace_id"]   = e.namespace_id   ? json(*e.namespace_id)   : json(nullptr);
    j["created_at"]     = e.created_at;
    return j;
}

// GET /traces/{session_id} body (§8.1). Pagination (total_count/has_next/
// next_offset) is computed by the writer and carried on TraceSession; trace_count
// is the rows in THIS page. meta carries the §8.1 soft-limit warning.
json TracesResponseToJson(const TraceSession& s, bool size_warning) {
    json traces = json::array();
    for (const auto& e : s.traces) traces.push_back(TraceEntryToJson(e));

    json body;
    body["session_id"]  = s.session_id;
    body["trace_count"] = s.trace_count;           // rows in this page
    body["traces"]      = std::move(traces);
    json meta;
    meta["session_start"]         = s.session_start;
    meta["session_end"]           = s.session_end;
    meta["total_duration_ms"]     = s.total_duration_ms;
    meta["total_count"]           = s.total_count;  // total matching the filter
    meta["has_next"]              = s.has_next;
    meta["next_offset"]           = s.has_next ? json(s.next_offset) : json(nullptr);
    meta["response_size_warning"] = size_warning;
    body["meta"] = std::move(meta);
    return body;
}

// GET /interactions/{id}/sources body (§8.2). highlight_ranges is deliberately
// absent in CE (Ent writes it to interaction_sources_extension).
json SourcesViewToJson(const InteractionSourcesView& v) {
    json sources = json::array();
    for (const auto& s : v.sources) {
        json js;
        js["source_id"]       = s.source_block_id;
        js["source_type"]     = s.source_type;
        js["snippet"]         = s.snippet;
        js["relevance_score"] = s.relevance_score;
        sources.push_back(std::move(js));
    }
    json body;
    body["interaction_id"]        = v.interaction_id;
    body["source_count"]          = v.source_count;
    body["deleted_sources_count"] = v.deleted_sources_count;
    body["sources"]               = std::move(sources);
    return body;
}

// GET /interactions body (§8.3). query_text returned in full (not truncated);
// namespace_id is the real namespace_name column.
json InteractionListToJson(const InteractionListView& v) {
    json items = json::array();
    for (const auto& it : v.interactions) {
        json ji;
        ji["interaction_id"] = it.interaction_id;
        ji["session_id"]     = it.session_id;
        ji["user_id"]        = it.user_id;
        ji["namespace_id"]   = it.namespace_id;
        ji["role"]           = it.role;
        ji["query_text"]     = it.query_text;
        ji["query_type"]     = it.query_type;
        ji["created_at"]     = it.created_at;
        items.push_back(std::move(ji));
    }
    json body;
    body["interactions"] = std::move(items);
    json meta;
    meta["total_count"] = v.total_count;
    meta["has_next"]    = v.has_next;
    meta["next_offset"] = v.has_next ? json(v.next_offset) : json(nullptr);
    body["meta"] = std::move(meta);
    return body;
}

// ---- shared route bodies (called by both the static-handler + per-NS paths) ----

// GET /api/v1/traces/:session_id body (§8.1). `traces` is built once (static path)
// or per-request over the NS memory.db (per-NS path).
void ServeTraces(agent_trace::TracesHandler& traces, const httplib::Request& req,
                 httplib::Response& res, const RequestContext& rctx) {
    ParseAndInstallObservabilityHeaders(req, res);
    auto sid_it = req.path_params.find("session_id");
    if (sid_it == req.path_params.end() || sid_it->second.empty()) {
        WriteF13InvalidFilter(res, rctx.request_id, "session_id",
                              "must be provided in the path", "");
        return;
    }
    const std::string& session_id = sid_it->second;
    TraceFilter filter;
    std::string bf, br, bv;
    if (!ParseTimestampParam(req, "from_timestamp", &filter.from_timestamp, &bf, &br, &bv) ||
        !ParseTimestampParam(req, "to_timestamp", &filter.to_timestamp, &bf, &br, &bv) ||
        !ParseIntParam(req, "limit", &filter.limit, &bf, &br, &bv) ||
        !ParseIntParam(req, "offset", &filter.offset, &bf, &br, &bv)) {
        WriteF13InvalidFilter(res, rctx.request_id, bf, br, bv);
        return;
    }
    if (req.has_param("status")) {
        const std::string st = req.get_param_value("status");
        if (st != "success" && st != "failed" && st != "cancelled" &&
            st != "session_timeout") {
            WriteF13InvalidFilter(res, rctx.request_id, "status",
                "must be one of success|failed|cancelled|session_timeout", st);
            return;
        }
        filter.status = st;
    }
    auto r = traces.GetSession(session_id, filter, MakeRequesterContext(rctx));
    if (!r.ok()) {
        WriteF13Error(res, r.status(), rctx.request_id, session_id);
        return;
    }
    WriteJsonResponse(res, 200,
        TracesResponseToJson(r.value().session, r.value().response_size_warning),
        rctx.request_id);
}

// GET /api/v1/interactions/:id/sources body (§8.2).
void ServeSources(agent_trace::InteractionsHandler& inter, const httplib::Request& req,
                  httplib::Response& res, const RequestContext& rctx) {
    ParseAndInstallObservabilityHeaders(req, res);
    auto id_it = req.path_params.find("id");
    if (id_it == req.path_params.end() || id_it->second.empty()) {
        WriteF13InvalidFilter(res, rctx.request_id, "interaction_id",
                              "must be provided in the path", "");
        return;
    }
    const std::string& interaction_id = id_it->second;
    auto r = inter.GetSources(interaction_id, MakeRequesterContext(rctx));
    if (!r.ok()) {
        WriteF13Error(res, r.status(), rctx.request_id, interaction_id);
        return;
    }
    WriteJsonResponse(res, 200, SourcesViewToJson(r.value()), rctx.request_id);
}

// GET /api/v1/interactions body (§8.3).
void ServeListInteractions(agent_trace::InteractionsHandler& inter,
                           const httplib::Request& req, httplib::Response& res,
                           const RequestContext& rctx) {
    ParseAndInstallObservabilityHeaders(req, res);
    InteractionListFilter filter;
    std::string bf, br, bv;
    if (!ParseTimestampParam(req, "from_timestamp", &filter.from_timestamp, &bf, &br, &bv) ||
        !ParseTimestampParam(req, "to_timestamp", &filter.to_timestamp, &bf, &br, &bv) ||
        !ParseIntParam(req, "limit", &filter.limit, &bf, &br, &bv) ||
        !ParseIntParam(req, "offset", &filter.offset, &bf, &br, &bv)) {
        WriteF13InvalidFilter(res, rctx.request_id, bf, br, bv);
        return;
    }
    if (req.has_param("user_id"))      filter.user_id = req.get_param_value("user_id");
    if (req.has_param("session_id"))   filter.session_id = req.get_param_value("session_id");
    if (req.has_param("namespace_id")) filter.namespace_id = req.get_param_value("namespace_id");
    if (req.has_param("sort_order")) {
        std::string so = req.get_param_value("sort_order");
        std::transform(so.begin(), so.end(), so.begin(),
                       [](unsigned char c) { return std::toupper(c); });
        if (so != "ASC" && so != "DESC") {
            WriteF13InvalidFilter(res, rctx.request_id, "sort_order",
                                  "must be ASC|DESC", req.get_param_value("sort_order"));
            return;
        }
        filter.sort_order = so;
    }
    auto r = inter.ListInteractions(filter, MakeRequesterContext(rctx));
    if (!r.ok()) {
        WriteF13Error(res, r.status(), rctx.request_id, /*id_value=*/"");
        return;
    }
    WriteJsonResponse(res, 200, InteractionListToJson(r.value()), rctx.request_id);
}

}  // namespace

void RegisterObservabilityRoutes(httplib::Server& server,
                                 agent_trace::TracesHandler& traces,
                                 agent_trace::InteractionsHandler& inter,
                                 ApiKeyAuth& auth) {
    server.Get("/api/v1/traces/:session_id", WithAuth(auth, kPermRead,
        [&traces](const httplib::Request& req, httplib::Response& res,
                  const RequestContext& rctx) { ServeTraces(traces, req, res, rctx); }));
    server.Get("/api/v1/interactions/:id/sources", WithAuth(auth, kPermRead,
        [&inter](const httplib::Request& req, httplib::Response& res,
                 const RequestContext& rctx) { ServeSources(inter, req, res, rctx); }));
    server.Get("/api/v1/interactions", WithAuth(auth, kPermRead,
        [&inter](const httplib::Request& req, httplib::Response& res,
                 const RequestContext& rctx) { ServeListInteractions(inter, req, res, rctx); }));
}

namespace {

// Resolve the namespace that selects the per-NS memory.db. `param_name` is the query
// param to read ("namespace" for the path-id routes; "namespace_id" for GET
// /interactions which reuses its existing filter param — the contract addendum). On
// miss, write the GEN-Agent 400 envelope and return false (per-NS storage needs the
// namespace to pick the right memory.db).
bool RequireNamespaceParam(const httplib::Request& req, httplib::Response& res,
                           const RequestContext& rctx, const char* param_name,
                           std::string* ns) {
    *ns = req.get_param_value(param_name);
    if (ns->empty() && std::string(param_name) == "namespace") *ns = req.get_param_value("ns");
    if (ns->empty()) {
        WriteF13InvalidFilter(res, rctx.request_id, param_name,
                              "query param is required (per-NS observability storage)", "");
        return false;
    }
    return true;
}

}  // namespace

void RegisterObservabilityRoutesPerNs(httplib::Server& server,
                                      resource::INamespacePool& pool,
                                      std::shared_ptr<IGlobalConfig> global_config,
                                      ApiKeyAuth& auth) {
    // For each route: require ?namespace=, acquire the façade, build the F13 handlers
    // over THIS namespace's memory.db (interaction_log + agent_trace +
    // interaction_sources all live there), then delegate to the shared Serve* body.
    auto with_ns = [&pool, global_config](
        const httplib::Request& req, httplib::Response& res, const RequestContext& rctx,
        const char* ns_param,
        const std::function<void(agent_trace::TracesHandler&,
                                 agent_trace::InteractionsHandler&)>& fn) {
        std::string ns;
        if (!RequireNamespaceParam(req, res, rctx, ns_param, &ns)) return;
        resource::NamespaceFacade facade(pool, ns);
        if (Status acq = facade.Acquire(); !acq.ok()) {
            WriteF13Error(res, Status::NotFound("CX_ERR_F13_SESSION_NOT_FOUND: namespace '" +
                                                ns + "' not found"),
                          rctx.request_id, ns);
            return;
        }
        sqlite3* mem_db = facade.memory().db_handle();
        auto writer = std::make_shared<agent_trace::AgentTraceWriterImpl>(mem_db, global_config);
        agent_trace::TracesHandler traces(writer, mem_db);
        agent_trace::InteractionsHandler inter(mem_db);
        fn(traces, inter);
    };

    server.Get("/api/v1/traces/:session_id", WithAuth(auth, kPermRead,
        [with_ns](const httplib::Request& req, httplib::Response& res,
                  const RequestContext& rctx) {
            with_ns(req, res, rctx, "namespace",
                    [&](agent_trace::TracesHandler& t, agent_trace::InteractionsHandler&) {
                        ServeTraces(t, req, res, rctx);
                    });
        }));

    server.Get("/api/v1/interactions/:id/sources", WithAuth(auth, kPermRead,
        [with_ns](const httplib::Request& req, httplib::Response& res,
                  const RequestContext& rctx) {
            with_ns(req, res, rctx, "namespace",
                    [&](agent_trace::TracesHandler&, agent_trace::InteractionsHandler& i) {
                        ServeSources(i, req, res, rctx);
                    });
        }));

    // GET /interactions reuses its existing `namespace_id` filter param as the
    // per-NS DB selector (contract addendum) — not the `namespace` param.
    server.Get("/api/v1/interactions", WithAuth(auth, kPermRead,
        [with_ns](const httplib::Request& req, httplib::Response& res,
                  const RequestContext& rctx) {
            with_ns(req, res, rctx, "namespace_id",
                    [&](agent_trace::TracesHandler&, agent_trace::InteractionsHandler& i) {
                        ServeListInteractions(i, req, res, rctx);
                    });
        }));
}

// ===== CORS =================================================================

namespace {

constexpr const char* kCorsAllowMethods = "GET,POST,PUT,DELETE,PATCH,OPTIONS";
constexpr const char* kCorsAllowHeaders =
    "Authorization,X-API-Key,Content-Type,X-Session-Id,X-Trace-Id,X-Agent-Id";
constexpr const char* kCorsMaxAge = "86400";  // 24h preflight cache

// A loopback origin is always allowed (local Agent / dev tooling): exactly
// http://localhost, http://127.0.0.1, or http://[::1], optionally with a :port.
// We require the http scheme + an exact host match (a port, if present, is any
// digits) so "http://localhost.evil.com" does NOT match.
bool IsLoopbackOrigin(const std::string& origin) {
    static const char* kPrefixes[] = {
        "http://localhost", "http://127.0.0.1", "http://[::1]"};
    for (const char* p : kPrefixes) {
        const std::string prefix(p);
        if (origin.size() < prefix.size()) continue;
        if (origin.compare(0, prefix.size(), prefix) != 0) continue;
        const std::string rest = origin.substr(prefix.size());
        if (rest.empty()) return true;             // exact host, no port
        if (rest[0] != ':') continue;              // must be a :port, not ".evil"
        bool all_digits = rest.size() > 1;
        for (size_t i = 1; i < rest.size(); ++i)
            if (!std::isdigit(static_cast<unsigned char>(rest[i]))) all_digits = false;
        if (all_digits) return true;
    }
    return false;
}

// The Origin is allowed iff it is loopback or appears verbatim in the configured
// allow-list (an empty list = loopback/same-origin only).
bool IsOriginAllowed(const std::string& origin,
                     const std::vector<std::string>& allowed_origins) {
    if (origin.empty()) return false;
    if (IsLoopbackOrigin(origin)) return true;
    return std::find(allowed_origins.begin(), allowed_origins.end(), origin) !=
           allowed_origins.end();
}

// Attach the CORS response headers for an allowed origin. Echoes the specific
// Origin (never "*", so credentialed requests work) + Vary: Origin.
void SetCorsHeaders(httplib::Response& res, const std::string& origin) {
    res.set_header("Access-Control-Allow-Origin", origin);
    res.set_header("Access-Control-Allow-Methods", kCorsAllowMethods);
    res.set_header("Access-Control-Allow-Headers", kCorsAllowHeaders);
    res.set_header("Access-Control-Max-Age", kCorsMaxAge);
    res.set_header("Vary", "Origin");
}

}  // namespace

void InstallCors(httplib::Server& server,
                 const std::vector<std::string>& allowed_origins) {
    // Preflight: a catch-all OPTIONS *route* (not a pre-routing handler). httplib
    // dispatches OPTIONS from its own handler table, so this answers every preflight
    // WITHOUT consuming the single set_pre_routing_handler slot — which the
    // admin_guard (main.cpp) already owns. For an allowed origin we answer 204 + the
    // CORS headers; otherwise 204 with no ACAO (the browser then blocks the call).
    server.Options(R"(.*)",
        [allowed_origins](const httplib::Request& req, httplib::Response& res) {
            const std::string origin = req.get_header_value("Origin");
            if (IsOriginAllowed(origin, allowed_origins)) {
                SetCorsHeaders(res, origin);
            }
            res.status = 204;  // No Content — preflight handled
        });

    // Actual responses: a post-routing handler adds the ACAO header for an allowed
    // origin on every routed response (so GET /traces etc. are browser-readable
    // cross-origin when configured). NOTE: httplib has a single post-routing slot —
    // no other module currently uses it; if one is added later, compose them.
    server.set_post_routing_handler(
        [allowed_origins](const httplib::Request& req, httplib::Response& res) {
            const std::string origin = req.get_header_value("Origin");
            if (IsOriginAllowed(origin, allowed_origins)) {
                SetCorsHeaders(res, origin);
            }
        });
}

}  // namespace cortrix
