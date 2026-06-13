#pragma once
#include <memory>
#include <string>

#include "cortrix/agent_trace/agent_trace_writer.h"
#include "cortrix/agent_trace/interactions_handler.h"  // RequesterContext
#include "cortrix/common/result.h"

struct sqlite3;

namespace cortrix::agent_trace {

/// Soft cap on the serialized response size (F13 §8.1 — 1MB soft limit). When the
/// returned page's rough byte estimate exceeds this, TraceSession.response_size_warning
/// is set so the Agent knows to paginate. (Carried on the handler result, below.)
constexpr int64_t kResponseSizeSoftLimitBytes = 1024 * 1024;

/// GET /api/v1/traces/{session_id} result (F13 §8.1). Wraps the writer's
/// TraceSession plus the §8.1 meta extras the handler computes (the soft-limit
/// warning). The permission decision is made here, not in the writer.
struct TracesResponse {
    TraceSession session;
    bool response_size_warning = false;   ///< §8.1 — page estimate > 1MB soft limit
};

/// CE business logic for GET /api/v1/traces/{session_id} (F13 §8.1, S4). Owns the
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
    /// @param writer  the agent_trace writer (query sink).
    /// @param db      borrowed handle to the DB holding interaction_log (for the
    ///                session->owner lookup). Not owned. May be null in tests that
    ///                only exercise admin paths (then ownership is "unknown").
    TracesHandler(std::shared_ptr<IAgentTraceWriter> writer, sqlite3* db);

    /// GET /traces/{session_id}. Permission-checks, then returns the filtered +
    /// paginated session. Errors carried as a Status with the CX_ERR_F13_* token.
    Result<TracesResponse> GetSession(const std::string& session_id,
                                      const TraceFilter& filter,
                                      const RequesterContext& ctx);

private:
    /// Resolve the session's owner user_id via interaction_log (session_id ->
    /// user_id). Returns {found, user_id}. `found` is false when no interaction_log
    /// row references the session (then ownership is unknown). A NULL user_id is
    /// reported as found with an empty string.
    struct Owner { bool found = false; std::string user_id; };
    Owner ResolveOwner(const std::string& session_id);

    std::shared_ptr<IAgentTraceWriter> writer_;
    sqlite3* db_;  ///< borrowed (not owned)
};

}  // namespace cortrix::agent_trace
