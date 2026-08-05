#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "cortrix/agent_trace/agent_trace_writer.h"
#include "cortrix/agent_trace/interactions_handler.h"  // RequesterContext
#include "cortrix/common/result.h"

struct sqlite3;

namespace cortrix::agent_trace {

/// Soft cap on the serialized response size (1MB soft limit). When the
/// returned page's rough byte estimate exceeds this, TraceSession.response_size_warning
/// is set so the Agent knows to paginate. (Carried on the handler result, below.)
constexpr int64_t kResponseSizeSoftLimitBytes = 1024 * 1024;

/// GET /api/v1/traces/{session_id} result. Wraps the writer's
/// TraceSession plus the §8.1 meta extras the handler computes (the soft-limit
/// warning). The permission decision is made here, not in the writer.
struct TracesResponse {
    TraceSession session;
    bool response_size_warning = false;   ///< §8.1 — page estimate > 1MB soft limit
};

/// CE business logic for GET /api/v1/traces/{session_id}. Owns the
/// permission decision (the writer just filters + paginates):
///   - the session's owner is resolved via interaction_log (session_id ->
///     user_id) — agent_trace itself carries no user_id, and the §11 closed loop ties
///     interactions/traces by session_id;
///   - non-admin reading a session they do not own -> CX_ERR_F13_UNAUTHORIZED
///     (anti-leak, §8.1);
///   - admin reading a session with no traces -> CX_ERR_F13_SESSION_NOT_FOUND;
///   - otherwise delegate to writer.Query and tag the soft-limit warning.
///
/// Standalone: pure logic over the writer + a borrowed DB handle (for the
/// session->owner lookup). The HTTP route registration is D3.5.
class TracesHandler {
public:
    /// Resolution of a session's owner (session_id -> user_id). `found` is false when
    /// no record ties the session to a user (then ownership is "unknown"). A NULL
    /// user_id is reported as found with an empty string.
    struct Owner { bool found = false; std::string user_id; };

    /// Strategy for resolving a session's owner. Used by the global GET /traces wiring
    /// (TC4): agent_trace lives in cortrix_global.db but the session->user mapping
    /// (interaction_log / memory_sessions) is per-NS, so a global trace read resolves
    /// ownership out-of-band (read the session's namespace_id off the global
    /// agent_trace, then look up user_id in that namespace's memory.db). When unset,
    /// the handler falls back to the borrowed `db` interaction_log query (the path
    /// used when the owner source is co-located with the writer, e.g. tests).
    using OwnerResolver = std::function<Owner(const std::string& session_id)>;

    /// @param writer  the agent_trace writer (query sink).
    /// @param db      borrowed handle to the DB holding interaction_log (for the
    ///                session->owner lookup). Not owned. May be null in tests that
    ///                only exercise admin paths (then ownership is "unknown"), or in
    ///                the TC4 global wiring (which sets an OwnerResolver instead).
    TracesHandler(std::shared_ptr<IAgentTraceWriter> writer, sqlite3* db);

    /// Install an out-of-band owner resolver (global wiring). When set it
    /// supersedes the co-located `db` interaction_log lookup — agent_trace is global
    /// but the session->user mapping is per-NS, so the global GET /traces resolves
    /// ownership through this strategy. Returns *this for call-chaining at the wiring.
    TracesHandler& SetOwnerResolver(OwnerResolver owner_resolver) {
        owner_resolver_ = std::move(owner_resolver);
        return *this;
    }

    /// GET /traces/{session_id}. Permission-checks, then returns the filtered +
    /// paginated session. Errors carried as a Status with the CX_ERR_F13_* token.
    Result<TracesResponse> GetSession(const std::string& session_id,
                                      const TraceFilter& filter,
                                      const RequesterContext& ctx);

private:
    /// Resolve the session's owner. Uses `owner_resolver_` when set (global wiring),
    /// else the borrowed-`db` interaction_log query (co-located wiring).
    Owner ResolveOwner(const std::string& session_id);
    /// The co-located interaction_log query (session_id -> user_id over `db_`).
    Owner ResolveOwnerFromDb(const std::string& session_id);

    std::shared_ptr<IAgentTraceWriter> writer_;
    sqlite3* db_;  ///< borrowed (not owned)
    OwnerResolver owner_resolver_;  ///< when set, supersedes the db_ lookup (TC4 global)
};

}  // namespace cortrix::agent_trace
