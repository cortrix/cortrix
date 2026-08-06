#include "cortrix/memory/opt_out_manager.h"

#include <chrono>
#include <cstdio>
#include <ctime>

#include "cortrix/memory/memory_opt_out_metrics.h"
#include "cortrix/memory/memory_store.h"

namespace cortrix::memory::immunity {

using observability::IOperationLogger;
using observability::OperationLogEntry;
using observability::TraceContext;

namespace {

// ISO 8601 UTC millis — same format as MemoryStore::NowISO8601, kept local so the
// manager does not depend on a store private. (D3.5 may unify these.)
std::string NowISO8601() {
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf;
    gmtime_r(&t, &tm_buf);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
        tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
        tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec,
        static_cast<int>(ms.count()));
    return std::string(buf);
}

// OptOutActor → the low-cardinality metric label (D4 enum collapsed to actor class).
MemoryOptOutMetrics::TriggeredBy MetricTrigger(OptOutActor actor) {
    switch (actor) {
        case OptOutActor::kUserManual: return MemoryOptOutMetrics::TriggeredBy::kUser;
        case OptOutActor::kAgentAuto:  return MemoryOptOutMetrics::TriggeredBy::kAgent;
        case OptOutActor::kSystemAuto: return MemoryOptOutMetrics::TriggeredBy::kSystem;
        case OptOutActor::kTest:       return MemoryOptOutMetrics::TriggeredBy::kSystem;
    }
    return MemoryOptOutMetrics::TriggeredBy::kSystem;
}

// Record one operation_log entry for an opt-out action (GEN-OperationLog). No-op
// when no logger is wired (standalone). action = {resource}_{verb} (≤32, lower, '_').
void RecordOplog(const std::shared_ptr<IOperationLogger>& logger,
                 const char* action,
                 const std::string& session_id,
                 const std::string& summary,
                 const TraceContext* ctx) {
    if (!logger) return;
    OperationLogEntry entry;
    entry.action = action;
    entry.resource_type = "memory";
    entry.resource_id = session_id;
    if (!summary.empty()) entry.summary = summary;
    logger->Log(entry, ctx);
}

}  // namespace

const char* ToString(OptOutActor actor) {
    return OptOutManager::ActorString(actor);
}

OptOutManager::OptOutManager(std::shared_ptr<ISessionOptOutStore> store,
                             std::shared_ptr<IOperationLogger> op_logger,
                             bool enabled)
    : store_(std::move(store)),
      op_logger_(std::move(op_logger)),
      enabled_(enabled) {}

const char* OptOutManager::ActorString(OptOutActor actor) {
    switch (actor) {
        case OptOutActor::kUserManual: return "user_manual";
        case OptOutActor::kAgentAuto:  return "agent_auto";
        case OptOutActor::kSystemAuto: return "system_auto";
        case OptOutActor::kTest:       return "test";
    }
    return "user_manual";
}

bool OptOutManager::IsValidSessionId(const std::string& session_id) {
    // §11.x boundary matrix: reject empty / structurally invalid ids. A permissive
    // "recognizable id" check — 8..64 chars over [0-9A-Za-z_-] — so the real client
    // ids MemoryStore accepts all pass: its own UUID v4 "8-4-4-4-12", ULIDs, and the
    // design's own `s_<digits>` session ids (§5.1). Empty strings and obvious junk
    // (spaces, control chars, oversize) are rejected.
    const size_t n = session_id.size();
    if (n < 8 || n > 64) return false;
    for (char c : session_id) {
        const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                        (c >= 'A' && c <= 'Z') || c == '-' || c == '_';
        if (!ok) return false;
    }
    return true;
}

Result<OptOutResult> OptOutManager::OptOut(const std::string& session_id,
                                           OptOutActor actor,
                                           const std::string& reason,
                                           const TraceContext* ctx) {
    if (!IsValidSessionId(session_id)) {
        return MemoryOptOutStatus(MemoryOptOutErrorCode::kInvalidSessionId,
                           "session_id is not a recognizable ULID/UUID");
    }
    if (!enabled_) {
        return MemoryOptOutStatus(MemoryOptOutErrorCode::kOptOutDisabled,
                           "memory_opt_out.enabled is false");
    }

    Result<SessionOptOutState> state = store_->GetOptOutState(session_id);
    if (!state.ok()) return state.status();  // store failure → propagate

    if (!state->exists) {
        return MemoryOptOutStatus(MemoryOptOutErrorCode::kSessionNotFound, session_id);
    }
    if (state->opted_out) {
        // ARCH §4.1.11: a repeat opt-out is ALREADY_OPTED_OUT (409). The §11.x
        // idempotent-200 variant is an HTTP-handler concern (D3.5).
        return MemoryOptOutStatus(MemoryOptOutErrorCode::kAlreadyOptedOut, session_id);
    }

    const std::string now = NowISO8601();
    const char* by = ActorString(actor);
    if (Status s = store_->SetOptOut(session_id, now, by); !s.ok()) {
        return s;
    }

    MemoryOptOutMetrics::Instance().RecordOptOut(MetricTrigger(actor));
    std::string summary = std::string("opt-out session ") + session_id;
    if (!reason.empty()) summary += " (" + reason + ")";
    RecordOplog(op_logger_, "memory_opt_out", session_id, summary, ctx);

    OptOutResult result;
    result.session_id = session_id;
    result.opt_out_at = now;
    result.opted_out_by = by;
    return result;
}

Result<RevokeResult> OptOutManager::OptOutRevoke(const std::string& session_id,
                                                 const std::string& reason,
                                                 const TraceContext* ctx) {
    if (!IsValidSessionId(session_id)) {
        return MemoryOptOutStatus(MemoryOptOutErrorCode::kInvalidSessionId,
                           "session_id is not a recognizable ULID/UUID");
    }
    if (!enabled_) {
        return MemoryOptOutStatus(MemoryOptOutErrorCode::kOptOutDisabled,
                           "memory_opt_out.enabled is false");
    }
    if (reason.empty()) {
        // §6.2 revoke requestBody: reason is required (recorded in operation_log).
        return MemoryOptOutStatus(MemoryOptOutErrorCode::kInvalidSessionId,
                           "revoke reason is required");
    }

    Result<SessionOptOutState> state = store_->GetOptOutState(session_id);
    if (!state.ok()) return state.status();

    if (!state->exists) {
        return MemoryOptOutStatus(MemoryOptOutErrorCode::kSessionNotFound, session_id);
    }
    if (!state->opted_out) {
        return MemoryOptOutStatus(MemoryOptOutErrorCode::kNotOptedOut, session_id);
    }

    if (Status s = store_->ClearOptOut(session_id); !s.ok()) {
        return s;
    }

    MemoryOptOutMetrics::Instance().RecordOptOutRevoke();
    RecordOplog(op_logger_, "memory_opt_out_revoke", session_id,
                std::string("revoke opt-out session ") + session_id + " (" + reason + ")",
                ctx);

    RevokeResult result;
    result.session_id = session_id;
    result.revoked_at = NowISO8601();
    return result;
}

Result<bool> OptOutManager::is_session_opted_out(const std::string& session_id,
                                                 const TraceContext* /*ctx*/) {
    // NOT gated by enabled_ — existing stamps must be honored even if new opt-outs
    // are disabled. An absent session is benign (false), not an error.
    Result<SessionOptOutState> state = store_->GetOptOutState(session_id);
    if (!state.ok()) return state.status();
    return state->exists && state->opted_out;
}

// ---------------------------------------------------------------------------
// MemoryStoreOptOutAdapter — real ISessionOptOutStore over cortrix::MemoryStore
// ---------------------------------------------------------------------------

MemoryStoreOptOutAdapter::MemoryStoreOptOutAdapter(::cortrix::MemoryStore& store)
    : store_(store) {}

Result<SessionOptOutState> MemoryStoreOptOutAdapter::GetOptOutState(
        const std::string& session_id) {
    SessionOptOutState st;
    bool exists = false;
    std::string opt_out_at;
    std::string opted_out_by;
    Status s = store_.SessionGetOptOut(session_id, &exists, &opt_out_at, &opted_out_by);
    if (!s.ok()) return s;
    st.exists = exists;
    st.opted_out = exists && !opt_out_at.empty();
    st.opt_out_at = opt_out_at;
    st.opted_out_by = opted_out_by;
    return st;
}

Status MemoryStoreOptOutAdapter::SetOptOut(const std::string& session_id,
                                           const std::string& opt_out_at,
                                           const std::string& opted_out_by) {
    return store_.SessionSetOptOut(session_id, opt_out_at, opted_out_by);
}

Status MemoryStoreOptOutAdapter::ClearOptOut(const std::string& session_id) {
    return store_.SessionClearOptOut(session_id);
}

}  // namespace cortrix::memory::immunity
