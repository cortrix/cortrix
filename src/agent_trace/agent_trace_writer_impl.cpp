#include <cstdint>
#include "cortrix/agent_trace/agent_trace_writer_impl.h"

#include <sqlite3.h>

#include <chrono>
#include <string>
#include <vector>

#include "cortrix/agent_trace/agent_trace_error.h"
#include "cortrix/agent_trace/agent_trace_metrics.h"

namespace cortrix::agent_trace {

namespace {

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

void BindOptText(sqlite3_stmt* stmt, int idx, const std::optional<std::string>& v) {
    if (v.has_value()) {
        sqlite3_bind_text(stmt, idx, v->c_str(), -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(stmt, idx);
    }
}

std::string ColText(sqlite3_stmt* stmt, int col) {
    const char* t = reinterpret_cast<const char*>(sqlite3_column_text(stmt, col));
    return t ? t : "";
}

std::optional<std::string> ColOptText(sqlite3_stmt* stmt, int col) {
    if (sqlite3_column_type(stmt, col) == SQLITE_NULL) return std::nullopt;
    return ColText(stmt, col);
}

// SELECT column order ReadRow() depends on (matches agent_trace, minus id
// which leads). Excludes nothing the AgentTraceEntry carries back.
constexpr const char* kSelectCols =
    "session_id, trace_id, agent_id, method, params, result_summary, "
    "duration_ms, source, status, error_code, namespace_id, created_at";

AgentTraceEntry ReadRow(sqlite3_stmt* stmt) {
    AgentTraceEntry e;
    e.session_id     = ColOptText(stmt, 0);
    e.trace_id       = ColOptText(stmt, 1);
    e.agent_id       = ColOptText(stmt, 2);
    e.method         = ColText(stmt, 3);
    e.params         = ColOptText(stmt, 4);
    e.result_summary = ColOptText(stmt, 5);
    e.duration_ms    = sqlite3_column_int(stmt, 6);
    e.source         = ColText(stmt, 7);
    e.status         = ColText(stmt, 8);
    e.error_code     = ColOptText(stmt, 9);
    e.namespace_id   = ColOptText(stmt, 10);
    e.created_at     = sqlite3_column_int64(stmt, 11);
    return e;
}

}  // namespace

AgentTraceWriterImpl::AgentTraceWriterImpl(sqlite3* db, std::shared_ptr<IGlobalConfig> config)
    : db_(db), config_(std::move(config)) {}

bool AgentTraceWriterImpl::InsertLocked(const AgentTraceEntry& entry,
                                        const observability::TraceContext* ctx) {
    static const char* kSql =
        "INSERT INTO agent_trace"
        "(session_id, trace_id, agent_id, method, params, result_summary, "
        " duration_ms, source, status, error_code, namespace_id, created_at) "
        "VALUES(?,?,?,?,?,?,?,?,?,?,?,?)";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, kSql, -1, &stmt, nullptr) != SQLITE_OK) {
        last_error_ = sqlite3_errmsg(db_);
        return false;
    }

    const int64_t ts = entry.created_at != 0 ? entry.created_at : NowMs();
    // Prefer the entry's own trace_id; fall back to the W3C ctx (GEN-Trace, C6).
    std::optional<std::string> trace = entry.trace_id;
    if (!trace.has_value() && ctx != nullptr && !ctx->trace_id.empty()) {
        trace = ctx->trace_id;
    }

    BindOptText(stmt, 1, entry.session_id);
    BindOptText(stmt, 2, trace);
    BindOptText(stmt, 3, entry.agent_id);
    sqlite3_bind_text(stmt, 4, entry.method.c_str(), -1, SQLITE_TRANSIENT);
    BindOptText(stmt, 5, entry.params);
    BindOptText(stmt, 6, entry.result_summary);
    sqlite3_bind_int(stmt, 7, entry.duration_ms);
    sqlite3_bind_text(stmt, 8, entry.source.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 9, entry.status.c_str(), -1, SQLITE_TRANSIENT);
    BindOptText(stmt, 10, entry.error_code);
    BindOptText(stmt, 11, entry.namespace_id);
    sqlite3_bind_int64(stmt, 12, ts);

    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        last_error_ = sqlite3_errmsg(db_);
        return false;
    }
    return true;
}

void AgentTraceWriterImpl::Write(const AgentTraceEntry& entry,
                                 const observability::TraceContext* ctx) {
    std::lock_guard<std::mutex> lock(mu_);
    const bool ok = InsertLocked(entry, ctx);
    // OBS_SPEC — count every write attempt by source + status. A failed DB
    // insert records last_error_ but never throws (C4 isolation).
    AgentTraceMetrics::WriteStatus ms = AgentTraceMetrics::WriteStatus::kSuccess;
    if (!ok) {
        ms = AgentTraceMetrics::WriteStatus::kFailed;
    } else if (entry.status == "failed") {
        ms = AgentTraceMetrics::WriteStatus::kFailed;
    } else if (entry.status == "cancelled") {
        ms = AgentTraceMetrics::WriteStatus::kCancelled;
    } else if (entry.status == "session_timeout") {
        ms = AgentTraceMetrics::WriteStatus::kSessionTimeout;
    }
    const auto src = entry.source == "mcp" ? AgentTraceMetrics::Source::kMcp
                                           : AgentTraceMetrics::Source::kHttp;
    AgentTraceMetrics::Instance().RecordWrite(src, ms);
    if (!ok) {
        AgentTraceMetrics::Instance().RecordWriteFailed(AgentTraceMetrics::Module::kAgentTrace);
    }
}

Result<TraceSession> AgentTraceWriterImpl::Query(const std::string& session_id,
                                                 const TraceFilter& filter,
                                                 const observability::TraceContext* /*ctx*/) {
    const int64_t t0 = NowMs();
    // ---- validate (permanent input faults → CX_ERR_TRACE_INVALID_FILTER) ----
    if (filter.limit < 1 || filter.limit > 200) {
        return AgentTraceStatus(AgentTraceErrorCode::kInvalidFilter,
                         "limit must be in [1,200], got " + std::to_string(filter.limit));
    }
    if (filter.offset < 0) {
        return AgentTraceStatus(AgentTraceErrorCode::kInvalidFilter,
                         "offset must be >= 0, got " + std::to_string(filter.offset));
    }
    if (filter.from_timestamp.has_value() && filter.to_timestamp.has_value() &&
        *filter.from_timestamp > *filter.to_timestamp) {
        return AgentTraceStatus(AgentTraceErrorCode::kInvalidFilter, "from_timestamp > to_timestamp");
    }

    std::lock_guard<std::mutex> lock(mu_);

    // ---- WHERE: always scoped to the path session_id, plus the filter dims ----
    std::string where = " WHERE session_id = ?";
    std::vector<std::string> text_binds;  // session_id first
    text_binds.push_back(session_id);
    std::vector<int64_t> int_binds;
    auto add_text = [&](const std::string& clause, const std::string& v) {
        where += clause;
        text_binds.push_back(v);
    };
    if (filter.trace_id.has_value())     add_text(" AND trace_id = ?", *filter.trace_id);
    if (filter.agent_id.has_value())     add_text(" AND agent_id = ?", *filter.agent_id);
    if (filter.status.has_value())       add_text(" AND status = ?", *filter.status);
    if (filter.namespace_id.has_value()) add_text(" AND namespace_id = ?", *filter.namespace_id);
    if (filter.from_timestamp.has_value()) {
        where += " AND created_at >= ?";
        int_binds.push_back(*filter.from_timestamp);
    }
    if (filter.to_timestamp.has_value()) {
        where += " AND created_at <= ?";
        int_binds.push_back(*filter.to_timestamp);
    }

    auto bind_predicates = [&](sqlite3_stmt* stmt) -> int {
        int idx = 1;
        for (const auto& t : text_binds)
            sqlite3_bind_text(stmt, idx++, t.c_str(), -1, SQLITE_TRANSIENT);
        for (int64_t v : int_binds)
            sqlite3_bind_int64(stmt, idx++, v);
        return idx;
    };

    // ---- total_count for the filtered page ----
    int64_t total_count = 0;
    {
        const std::string sql = "SELECT COUNT(*) FROM agent_trace" + where;
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
            last_error_ = sqlite3_errmsg(db_);
            return AgentTraceStatus(AgentTraceErrorCode::kInternal, last_error_);
        }
        bind_predicates(stmt);
        if (sqlite3_step(stmt) == SQLITE_ROW) total_count = sqlite3_column_int64(stmt, 0);
        sqlite3_finalize(stmt);
    }

    // offset past the end of a non-empty set is out of range.
    if (filter.offset > 0 && filter.offset >= total_count) {
        return AgentTraceStatus(AgentTraceErrorCode::kInvalidFilter,
                         "offset " + std::to_string(filter.offset) +
                             " >= total_count " + std::to_string(total_count));
    }

    TraceSession out;
    out.session_id = session_id;
    out.total_count = total_count;

    // ---- session-wide aggregate meta (NOT filtered by the query dims,
    //      meta is over the whole session: start / end / total_duration) ----
    {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_,
                "SELECT COALESCE(MIN(created_at),0), COALESCE(MAX(created_at),0), "
                "COALESCE(SUM(duration_ms),0) FROM agent_trace WHERE session_id = ?",
                -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, session_id.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                out.session_start = sqlite3_column_int64(stmt, 0);
                out.session_end = sqlite3_column_int64(stmt, 1);
                out.total_duration_ms = static_cast<int>(sqlite3_column_int64(stmt, 2));
            }
            sqlite3_finalize(stmt);
        }
    }

    // ---- page query (ordered by created_at then id, ascending = call order) ----
    {
        const std::string sql = "SELECT " + std::string(kSelectCols) +
                                " FROM agent_trace" + where +
                                " ORDER BY created_at ASC, id ASC LIMIT ? OFFSET ?";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
            last_error_ = sqlite3_errmsg(db_);
            return AgentTraceStatus(AgentTraceErrorCode::kInternal, last_error_);
        }
        int idx = bind_predicates(stmt);
        sqlite3_bind_int(stmt, idx++, filter.limit);
        sqlite3_bind_int(stmt, idx++, filter.offset);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            out.traces.push_back(ReadRow(stmt));
        }
        sqlite3_finalize(stmt);
    }

    out.trace_count = static_cast<int>(out.traces.size());
    out.next_offset = filter.offset + out.trace_count;
    out.has_next = out.next_offset < total_count;

    AgentTraceMetrics::Instance().ObserveQueryLatency(static_cast<int>(NowMs() - t0));
    return out;
}

void AgentTraceWriterImpl::OnSessionEnd(const std::string& /*session_id*/,
                                        const observability::TraceContext* /*ctx*/) {
    // Phase 1: the session_end agent_trace row is written by the caller via Write()
    // (OnConnectionClosed). No extra per-session state to finalize here; the
    // hook exists so an implementation (or Ent) can flush aggregates. No-op for CE.
}

void AgentTraceWriterImpl::CheckAndMarkTimeoutSessions(
    int idle_threshold_seconds, const observability::TraceContext* ctx) {
    std::lock_guard<std::mutex> lock(mu_);
    const int64_t cutoff = NowMs() - static_cast<int64_t>(idle_threshold_seconds) * 1000LL;

    // Find mcp sessions whose last activity is older than the idle threshold and
    // that have no terminal (session_end / session_timeout) row yet.
    std::vector<std::string> stale;
    {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, R"(
                SELECT a1.session_id
                FROM agent_trace a1
                WHERE a1.source = 'mcp' AND a1.session_id IS NOT NULL
                GROUP BY a1.session_id
                HAVING MAX(a1.created_at) < ?
                   AND SUM(CASE WHEN a1.method IN ('session_end','session_timeout')
                                THEN 1 ELSE 0 END) = 0
            )", -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(stmt, 1, cutoff);
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                const char* t = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                if (t) stale.push_back(t);
            }
            sqlite3_finalize(stmt);
        } else {
            last_error_ = sqlite3_errmsg(db_);
            return;
        }
    }

    for (const std::string& sid : stale) {
        AgentTraceEntry e;
        e.session_id = sid;
        e.method = "session_timeout";
        e.source = "mcp";
        e.status = "session_timeout";
        InsertLocked(e, ctx);
        AgentTraceMetrics::Instance().RecordWrite(
            AgentTraceMetrics::Source::kMcp,
            AgentTraceMetrics::WriteStatus::kSessionTimeout);
    }
}

void AgentTraceWriterImpl::Cleanup() {
    std::lock_guard<std::mutex> lock(mu_);
    last_cleanup_deleted_ = 0;

    const int retention_days = config_ ? config_->agent_trace_retention_days : 90;
    const int64_t cutoff = NowMs() - static_cast<int64_t>(retention_days) * 86400000LL;

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_,
            "DELETE FROM agent_trace WHERE created_at < ?", -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, cutoff);
        if (sqlite3_step(stmt) == SQLITE_DONE) last_cleanup_deleted_ = sqlite3_changes(db_);
        else last_error_ = sqlite3_errmsg(db_);
        sqlite3_finalize(stmt);
    } else {
        last_error_ = sqlite3_errmsg(db_);
    }
    last_cleanup_timestamp_ = NowMs();
}

int64_t AgentTraceWriterImpl::last_cleanup_deleted() const {
    std::lock_guard<std::mutex> lock(mu_);
    return last_cleanup_deleted_;
}

std::string AgentTraceWriterImpl::last_error() const {
    std::lock_guard<std::mutex> lock(mu_);
    return last_error_;
}

}  // namespace cortrix::agent_trace
