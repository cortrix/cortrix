#pragma once
#include <memory>
#include <string>
#include <vector>

struct sqlite3;

namespace httplib { class Server; }

namespace cortrix {

class ApiKeyAuth;
class IGlobalConfig;
namespace resource { class INamespacePool; }

namespace agent_trace {
class TracesHandler;
class InteractionsHandler;
}  // namespace agent_trace

/// Register the F13 Agent-Observability HTTP routes (F13 §8, S4/S5/S11). These are
/// the read-only query layer over the existing CE handlers — the handlers own the
/// permission decisions + DB access; this layer only adapts httplib <-> handler:
/// it parses the F13 identity headers (X-Session-Id/X-Trace-Id/X-Agent-Id) via the
/// shared HttpObservabilityMiddleware (installing the ObservabilityContext on the
/// thread-local + surfacing the per-header warning), derives the requester context
/// from the authenticated AuthContext (user_id + admin bit), parses + validates the
/// query params, and renders the result / the GEN-Agent CX_ERR_F13_* error body.
///
/// Endpoints (all WithAuth, Read permission):
///   GET /api/v1/traces/:session_id            -- session interaction trace list (§8.1)
///   GET /api/v1/interactions/:id/sources       -- retrieval source attribution (§8.2)
///   GET /api/v1/interactions                   -- Agent self-serve history query (§8.3)
///
/// @param server   the raw httplib server (routes registered on it).
/// @param traces   GET /traces/{session_id} business logic (owns permission + the
///                 session->owner lookup + writer.Query). Must outlive `server`.
/// @param inter    GET /interactions[/.../sources] business logic (owns permission +
///                 interaction_log/interaction_sources reads). Must outlive `server`.
/// @param auth     the API-key authenticator (WithAuth wrapper). Must outlive `server`.
void RegisterObservabilityRoutes(httplib::Server& server,
                                 agent_trace::TracesHandler& traces,
                                 agent_trace::InteractionsHandler& inter,
                                 ApiKeyAuth& auth);

// [F13 TC4] The old combined RegisterObservabilityRoutesPerNs (all 3 F13 routes over
// the per-NS memory.db, ?namespace= forced on /traces) was REMOVED. TC4 split
// agent_trace back to the global DB, so the live wiring is now the two functions below:
// RegisterTracesRoutesGlobal (trace read, global, no ?namespace=) +
// RegisterInteractionsRoutesPerNs (the two interaction reads, still per-NS).

/// [F13 TC4] Register GET /api/v1/traces/:session_id over the GLOBAL cortrix_global.db
/// (where agent_trace lives after TC4). A session's calls can span namespaces, so the
/// read is global and does NOT take `?namespace=`. Ownership for the §8.1 permission
/// check is resolved out-of-band: agent_trace carries no user_id and the session->user
/// mapping (interaction_log / memory_sessions) is per-NS, so the wiring reads the
/// session's namespace off the global agent_trace, then resolves user_id in that
/// namespace's memory.db via `pool` (a session belongs to one user).
///
/// @param server         the raw httplib server.
/// @param global_db      borrowed cortrix_global.db handle holding agent_trace. Not owned.
/// @param pool           NS pool, for the per-NS owner lookup. Must outlive `server`.
/// @param global_config  retention / config source for the writer.
/// @param auth           the API-key authenticator (WithAuth wrapper).
void RegisterTracesRoutesGlobal(httplib::Server& server,
                                sqlite3* global_db,
                                resource::INamespacePool& pool,
                                std::shared_ptr<IGlobalConfig> global_config,
                                ApiKeyAuth& auth);

/// [F13 TC4] Register the two per-NS interaction reads — GET /api/v1/interactions and
/// GET /api/v1/interactions/:id/sources — built PER-REQUEST over the request's
/// namespace memory.db (interaction_log + interaction_sources live there, unchanged by
/// TC4). `?namespace=<ns>` is REQUIRED on /interactions/:id/sources; GET /interactions
/// reuses its existing `namespace_id` filter param as the per-NS selector. (Split out
/// of the old combined RegisterObservabilityRoutesPerNs; the /traces route moved to
/// RegisterTracesRoutesGlobal.) No global_config arg — these reads need only the
/// per-NS memory.db handle (the retention config is the writer's concern, /traces only).
void RegisterInteractionsRoutesPerNs(httplib::Server& server,
                                     resource::INamespacePool& pool,
                                     ApiKeyAuth& auth);

/// Install a CORS layer on `server` (F13 topic 4 — "allowed headers" decision +
/// Lead cross-cutting decision). Registers an OPTIONS preflight handler and a
/// post-routing handler that, for every response, echoes the request Origin into
/// `Access-Control-Allow-Origin` ONLY when that Origin is allowed:
///   - it is listed in `allowed_origins`, OR
///   - it is a loopback origin (http://127.0.0.1[:port] / http://localhost[:port],
///     and the IPv6 loopback http://[::1][:port]).
/// An empty `allowed_origins` therefore means same-origin / loopback only — the
/// Agent-first server rarely needs CORS, so this is the safe default. When the
/// Origin is not allowed (or the request carries no Origin), no ACAO header is set
/// and the browser blocks the cross-origin read.
///
/// On an allowed origin the layer also sets:
///   Access-Control-Allow-Methods: GET,POST,PUT,DELETE,PATCH,OPTIONS
///   Access-Control-Allow-Headers: Authorization,X-API-Key,Content-Type,
///                                 X-Session-Id,X-Trace-Id,X-Agent-Id
///   Access-Control-Max-Age:       <fixed>
///   Vary: Origin   (the ACAO value depends on the request Origin)
///
/// Call AFTER the route registrars (the OPTIONS handler must win over any method
/// fallthrough). main.cpp passes config.security.cors_allowed_origins.
void InstallCors(httplib::Server& server,
                 const std::vector<std::string>& allowed_origins);

}  // namespace cortrix
