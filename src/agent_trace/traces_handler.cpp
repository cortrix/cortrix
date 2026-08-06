#include <cstdint>
#include "cortrix/agent_trace/traces_handler.h"

#include <sqlite3.h>

#include <utility>

#include "cortrix/agent_trace/agent_trace_error.h"
#include "cortrix/agent_trace/agent_trace_metrics.h"
#include "cortrix/agent_trace/observability_audit_log.h"

namespace cortrix::agent_trace {

namespace {

// Rough serialized-size estimate for one trace row (1MB soft limit). The
// variable-length fields dominate; a flat per-row overhead covers the fixed JSON
// keys/punctuation. This is intentionally an estimate (not a real serialize) —
// it only drives the "you should paginate" hint.
int64_t EstimateRowBytes(const AgentTraceEntry& e) {
    int64_t n = 160;  // fixed JSON scaffolding per row (keys + ids + numbers)
    if (e.params) n += static_cast<int64_t>(e.params->size());
    if (e.result_summary) n += static_cast<int64_t>(e.result_summary->size());
    return n;
}

}  // namespace

TracesHandler::TracesHandler(std::shared_ptr<IAgentTraceWriter> writer, sqlite3* db)
    : writer_(std::move(writer)), db_(db) {}

TracesHandler::Owner TracesHandler::ResolveOwner(const std::string& session_id) {
    // TC4 global wiring supplies an out-of-band resolver (agent_trace is global but
    // the session->user mapping is per-NS); when present it is authoritative.
    if (owner_resolver_) return owner_resolver_(session_id);
    return ResolveOwnerFromDb(session_id);
}

TracesHandler::Owner TracesHandler::ResolveOwnerFromDb(const std::string& session_id) {
    Owner o;
    if (db_ == nullptr) return o;  // no lookup source -> unknown
    sqlite3_stmt* stmt = nullptr;
    // interaction_log (MVP) ties session_id -> user_id; any one row's owner is the
    // session owner (a session belongs to one user).
    if (sqlite3_prepare_v2(db_,
            "SELECT user_id FROM interaction_log WHERE session_id = ? LIMIT 1",
            -1, &stmt, nullptr) != SQLITE_OK) {
        return o;  // table absent / error -> unknown
    }
    sqlite3_bind_text(stmt, 1, session_id.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        o.found = true;
        if (sqlite3_column_type(stmt, 0) != SQLITE_NULL) {
            const char* t = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            if (t) o.user_id = t;
        }
    }
    sqlite3_finalize(stmt);
    return o;
}

Result<TracesResponse> TracesHandler::GetSession(const std::string& session_id,
                                                 const TraceFilter& filter,
                                                 const RequesterContext& ctx) {
    // Permission. Resolve the session owner via interaction_log.
    const Owner owner = ResolveOwner(session_id);

    if (!ctx.is_admin) {
        // A non-admin may only read a session they own. If the owner is unknown or
        // someone else's, that is a cross-user attempt -> UNAUTHORIZED (anti-leak;
        // we deliberately do NOT reveal whether the session exists).
        if (!owner.found || owner.user_id != ctx.requester_user_id) {
            return AgentTraceStatus(AgentTraceErrorCode::kUnauthorized,
                             "cross-user trace access requires admin");
        }
    } else if (owner.found && owner.user_id != ctx.requester_user_id) {
        // forensics: an admin reading another user's session writes the
        // OBS_SPEC structured log (not operation_log, not a metric).
        ObservabilityAuditLog::Emit(ctx.requester_user_id, owner.user_id,
                                    ObservabilityAuditLog::Endpoint::kTraces);
    }

    // Delegate the filtered + paginated read to the writer.
    auto qr = writer_->Query(session_id, filter);
    if (!qr.ok()) {
        return qr.status();  // CX_ERR_TRACE_INVALID_FILTER / INTERNAL token preserved
    }
    TraceSession ts = std::move(qr.value());

    // admin reading a session that has no traces at all -> SESSION_NOT_FOUND
    // (admin cross-user nonexistent session). For a non-admin owner we keep
    // an empty page (the session is theirs; it is simply empty).
    if (ctx.is_admin && ts.total_count == 0 && !owner.found) {
        return AgentTraceStatus(AgentTraceErrorCode::kSessionNotFound,
                         "no session with id " + session_id);
    }

    TracesResponse out;
    // 1MB soft-limit warning over the returned page.
    int64_t bytes = 256;  // envelope (meta + braces)
    for (const auto& e : ts.traces) bytes += EstimateRowBytes(e);
    out.response_size_warning = bytes > kResponseSizeSoftLimitBytes;
    out.session = std::move(ts);

    AgentTraceMetrics::Instance().RecordTracesQuery(
        ctx.is_admin ? AgentTraceMetrics::Role::kAdmin : AgentTraceMetrics::Role::kUser,
        AgentTraceMetrics::Endpoint::kTraces);
    return out;
}

}  // namespace cortrix::agent_trace
