#include <cstdint>
#include "cortrix/observability/operation_logger_impl.h"

#include <sqlite3.h>

#include <chrono>
#include <string>
#include <vector>

#include "cortrix/observability/operation_log_error.h"
#include "cortrix/observability/oplog_metrics.h"

namespace cortrix::observability {

namespace {

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// Bind an optional<string> to a 1-based param: text when set, SQL NULL otherwise.
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

// Read an optional<string>: NULL column → nullopt, else the text.
std::optional<std::string> ColOptText(sqlite3_stmt* stmt, int col) {
    if (sqlite3_column_type(stmt, col) == SQLITE_NULL) return std::nullopt;
    return ColText(stmt, col);
}

// The SELECT column order Read() below depends on (matches operation_log §5.1).
constexpr const char* kSelectCols =
    "id, timestamp, user_id, action, namespace_id, resource_type, resource_id, "
    "summary, trace_id, session_id";

OperationLogEntry ReadRow(sqlite3_stmt* stmt) {
    OperationLogEntry e;
    e.id            = sqlite3_column_int64(stmt, 0);
    e.timestamp     = sqlite3_column_int64(stmt, 1);
    e.user_id       = ColText(stmt, 2);
    e.action        = ColText(stmt, 3);
    e.namespace_id  = ColOptText(stmt, 4);
    e.resource_type = ColText(stmt, 5);
    e.resource_id   = ColOptText(stmt, 6);
    e.summary       = ColOptText(stmt, 7);
    e.trace_id      = ColOptText(stmt, 8);
    e.session_id    = ColOptText(stmt, 9);
    return e;
}

}  // namespace

OperationLogger::OperationLogger(sqlite3* db, std::shared_ptr<IGlobalConfig> config)
    : db_(db), config_(std::move(config)) {}

bool OperationLogger::InsertLocked(const OperationLogEntry& entry,
                                   const TraceContext* ctx) {
    static const char* kSql =
        "INSERT INTO operation_log"
        "(timestamp, user_id, action, namespace_id, resource_type, resource_id, "
        " summary, trace_id, session_id) VALUES(?,?,?,?,?,?,?,?,?)";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, kSql, -1, &stmt, nullptr) != SQLITE_OK) {
        last_error_ = sqlite3_errmsg(db_);
        return false;
    }

    const int64_t ts = entry.timestamp != 0 ? entry.timestamp : NowMs();
    // Prefer the entry's own trace/session; fall back to the W3C ctx (GEN-Trace).
    std::optional<std::string> trace = entry.trace_id;
    std::optional<std::string> session = entry.session_id;
    if (!trace.has_value() && ctx != nullptr && !ctx->trace_id.empty()) {
        trace = ctx->trace_id;
    }

    sqlite3_bind_int64(stmt, 1, ts);
    sqlite3_bind_text(stmt, 2, entry.user_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, entry.action.c_str(), -1, SQLITE_TRANSIENT);
    BindOptText(stmt, 4, entry.namespace_id);
    sqlite3_bind_text(stmt, 5, entry.resource_type.c_str(), -1, SQLITE_TRANSIENT);
    BindOptText(stmt, 6, entry.resource_id);
    BindOptText(stmt, 7, entry.summary);
    BindOptText(stmt, 8, trace);
    BindOptText(stmt, 9, session);

    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        last_error_ = sqlite3_errmsg(db_);
        return false;
    }
    return true;
}

void OperationLogger::Log(const OperationLogEntry& entry, const TraceContext* ctx) {
    bool ok;
    {
        std::lock_guard<std::mutex> lock(mu_);
        ok = InsertLocked(entry, ctx);
    }
    // §11 cortrix_oplog_writes_total{action, resource_type}: count successful writes
    // only (operation_log records successful operations — §4.1). Recorded outside mu_
    // so the metric's own lock never nests under the DB lock.
    if (ok) {
        OplogMetrics::Instance().RecordWrite(entry.action, entry.resource_type);
    }
    // No-throw: a write failure records last_error_ (surfaced via Health()) but
    // never propagates — the Engine instrumentation site must not let observability break the
    // business path (C4 exception isolation).
}

void OperationLogger::BatchLog(const std::vector<OperationLogEntry>& entries,
                               const TraceContext* ctx) {
    if (entries.empty()) return;
    std::lock_guard<std::mutex> lock(mu_);

    // all-or-nothing in one transaction (topic 4 — SQLite transaction guarantee).
    if (sqlite3_exec(db_, "BEGIN", nullptr, nullptr, nullptr) != SQLITE_OK) {
        last_error_ = sqlite3_errmsg(db_);
        return;
    }
    bool ok = true;
    for (const auto& e : entries) {
        if (!InsertLocked(e, ctx)) { ok = false; break; }
    }
    sqlite3_exec(db_, ok ? "COMMIT" : "ROLLBACK", nullptr, nullptr, nullptr);
}

Result<OperationLogQueryResult> OperationLogger::Query(
    const OperationLogFilter& filter, const TraceContext* /*ctx*/) {
    const int64_t query_start_ms = NowMs();
    // §11 filter_dimensions label = number of active query filter fields (the 8 §6.1
    // query dimensions). Low-cardinality (0-8), bucketed by the metric recorder.
    int filter_dimensions = 0;
    if (filter.action.has_value())         ++filter_dimensions;
    if (filter.action_in.has_value())      ++filter_dimensions;
    if (filter.namespace_id.has_value())   ++filter_dimensions;
    if (filter.user_id.has_value())        ++filter_dimensions;
    if (filter.resource_type.has_value())  ++filter_dimensions;
    if (filter.trace_id.has_value())       ++filter_dimensions;
    if (filter.from_timestamp.has_value()) ++filter_dimensions;
    if (filter.to_timestamp.has_value())   ++filter_dimensions;

    // ---- validate (§7.2 permanent input faults) ----
    if (filter.limit < 1 || filter.limit > 200) {
        return OplogStatus(OplogErrorCode::kInvalidFilter,
                           "limit must be in [1,200], got " + std::to_string(filter.limit));
    }
    if (filter.offset < 0) {
        return OplogStatus(OplogErrorCode::kInvalidFilter,
                           "offset must be >= 0, got " + std::to_string(filter.offset));
    }
    if (filter.sort_order != "ASC" && filter.sort_order != "DESC") {
        return OplogStatus(OplogErrorCode::kInvalidFilter,
                           "sort_order must be ASC|DESC, got " + filter.sort_order);
    }
    if (filter.from_timestamp.has_value() && filter.to_timestamp.has_value() &&
        *filter.from_timestamp > *filter.to_timestamp) {
        return OplogStatus(OplogErrorCode::kInvalidTimestampRange,
                           "from > to");
    }
    if (filter.action_in.has_value() && filter.action_in->empty()) {
        return OplogStatus(OplogErrorCode::kInvalidFilter,
                           "action_in must be non-empty when present");
    }

    std::lock_guard<std::mutex> lock(mu_);

    // ---- build the WHERE clause + ordered bind list ----
    // Each predicate appends " AND <col> ..." and a bind value, so the WHERE and
    // the bind order stay in lockstep (avoids positional-param drift).
    std::string where = " WHERE 1=1";
    std::vector<std::string> text_binds;  // bound as text, in append order
    std::vector<int64_t> int_binds;       // bound as int64, after text (see below)
    // To keep binding simple, we bind text predicates first then int predicates,
    // and build the SQL in the same split order.
    auto add_text = [&](const std::string& clause, const std::string& v) {
        where += clause;
        text_binds.push_back(v);
    };
    if (filter.action.has_value())        add_text(" AND action = ?", *filter.action);
    if (filter.namespace_id.has_value())  add_text(" AND namespace_id = ?", *filter.namespace_id);
    if (filter.user_id.has_value())       add_text(" AND user_id = ?", *filter.user_id);
    if (filter.resource_type.has_value()) add_text(" AND resource_type = ?", *filter.resource_type);
    if (filter.trace_id.has_value())      add_text(" AND trace_id = ?", *filter.trace_id);
    if (filter.action_in.has_value()) {
        where += " AND action IN (";
        for (size_t i = 0; i < filter.action_in->size(); ++i) {
            where += (i == 0 ? "?" : ",?");
            text_binds.push_back((*filter.action_in)[i]);
        }
        where += ")";
    }
    // int predicates appended after all text predicates (binding split mirrors it)
    std::string int_where;
    if (filter.from_timestamp.has_value()) {
        int_where += " AND timestamp >= ?";
        int_binds.push_back(*filter.from_timestamp);
    }
    if (filter.to_timestamp.has_value()) {
        int_where += " AND timestamp <= ?";
        int_binds.push_back(*filter.to_timestamp);
    }
    where += int_where;

    // Binds a prepared stmt's text predicates [1..N] then int predicates
    // [N+1..N+M]; returns the next free 1-based index (for limit/offset).
    auto bind_predicates = [&](sqlite3_stmt* stmt) -> int {
        int idx = 1;
        for (const auto& t : text_binds)
            sqlite3_bind_text(stmt, idx++, t.c_str(), -1, SQLITE_TRANSIENT);
        for (int64_t v : int_binds)
            sqlite3_bind_int64(stmt, idx++, v);
        return idx;
    };

    // ---- total_count (for pagination meta + out-of-range check) ----
    int64_t total_count = 0;
    {
        const std::string count_sql = "SELECT COUNT(*) FROM operation_log" + where;
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, count_sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
            last_error_ = sqlite3_errmsg(db_);
            return OplogStatus(OplogErrorCode::kInternal, last_error_);
        }
        bind_predicates(stmt);
        if (sqlite3_step(stmt) == SQLITE_ROW) total_count = sqlite3_column_int64(stmt, 0);
        sqlite3_finalize(stmt);
    }

    // offset past the end of a non-empty result set is out-of-range (§7.2). An
    // offset of 0 on an empty set is a valid empty page (not an error).
    if (filter.offset > 0 && filter.offset >= total_count) {
        nlohmann::json sd = {{"offset", filter.offset}, {"total_count", total_count}};
        return OplogStatus(OplogErrorCode::kPaginationOutOfRange,
                           "offset " + std::to_string(filter.offset) +
                               " >= total_count " + std::to_string(total_count));
    }

    // ---- page query ----
    OperationLogQueryResult result;
    result.total_count = total_count;
    {
        const std::string sql = "SELECT " + std::string(kSelectCols) +
                                " FROM operation_log" + where +
                                " ORDER BY timestamp " + filter.sort_order +
                                ", id " + filter.sort_order + " LIMIT ? OFFSET ?";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
            last_error_ = sqlite3_errmsg(db_);
            return OplogStatus(OplogErrorCode::kInternal, last_error_);
        }
        int idx = bind_predicates(stmt);
        sqlite3_bind_int(stmt, idx++, filter.limit);
        sqlite3_bind_int(stmt, idx++, filter.offset);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            result.entries.push_back(ReadRow(stmt));
        }
        sqlite3_finalize(stmt);
    }

    result.next_offset = filter.offset + static_cast<int>(result.entries.size());
    result.has_next = result.next_offset < total_count;
    // §11 cortrix_oplog_query_latency_seconds{filter_dimensions}: observe the
    // successful read path latency (validation-reject paths return before the DB
    // work and are not query-latency samples).
    OplogMetrics::Instance().ObserveQueryLatency(
        filter_dimensions, static_cast<int>(NowMs() - query_start_ms));
    return result;
}

void OperationLogger::Cleanup() {
    const int64_t cleanup_start_ms = NowMs();
    int64_t deleted_age = 0;
    int64_t deleted_quota = 0;
    bool failed = false;
    int64_t final_rows = -1;

    {
        std::lock_guard<std::mutex> lock(mu_);
        last_cleanup_deleted_ = 0;

        const int retention_days = config_ ? config_->operation_log_retention_days : 30;
        const int64_t max_rows = config_ ? config_->operation_log_max_rows : 100000;
        const int64_t cutoff = NowMs() - static_cast<int64_t>(retention_days) * 86400000LL;

        // 1. age-based deletion (rows older than the retention window). A prepare
        //    failure (e.g. the table is absent) records last_error_ + the
        //    cleanup_failed metric just like a step failure — retention silently
        //    never running is exactly what Health() exists to surface.
        {
            sqlite3_stmt* stmt = nullptr;
            if (sqlite3_prepare_v2(db_,
                    "DELETE FROM operation_log WHERE timestamp < ?", -1, &stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_int64(stmt, 1, cutoff);
                if (sqlite3_step(stmt) == SQLITE_DONE) deleted_age += sqlite3_changes(db_);
                else { last_error_ = sqlite3_errmsg(db_); failed = true; }
                sqlite3_finalize(stmt);
            } else {
                last_error_ = sqlite3_errmsg(db_);
                failed = true;
            }
        }

        // 2. row-cap: delete the oldest rows beyond max_rows (0 = unbounded, Ent).
        //    Prepare failures on the retention path record last_error_ like step
        //    failures (a failed COUNT would otherwise silently skip the cap).
        if (max_rows > 0) {
            int64_t count = 0;
            sqlite3_stmt* cnt = nullptr;
            if (sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM operation_log", -1, &cnt, nullptr) == SQLITE_OK) {
                if (sqlite3_step(cnt) == SQLITE_ROW) count = sqlite3_column_int64(cnt, 0);
                sqlite3_finalize(cnt);
            } else {
                last_error_ = sqlite3_errmsg(db_);
                failed = true;
            }
            if (count > max_rows) {
                sqlite3_stmt* del = nullptr;
                if (sqlite3_prepare_v2(db_,
                        "DELETE FROM operation_log WHERE id IN "
                        "(SELECT id FROM operation_log ORDER BY timestamp ASC, id ASC LIMIT ?)",
                        -1, &del, nullptr) == SQLITE_OK) {
                    sqlite3_bind_int64(del, 1, count - max_rows);
                    if (sqlite3_step(del) == SQLITE_DONE) deleted_quota += sqlite3_changes(db_);
                    else { last_error_ = sqlite3_errmsg(db_); failed = true; }
                    sqlite3_finalize(del);
                } else {
                    last_error_ = sqlite3_errmsg(db_);
                    failed = true;
                }
            }
        }

        last_cleanup_deleted_ = deleted_age + deleted_quota;
        last_cleanup_timestamp_ = NowMs();

        // Post-sweep row count for the size_rows gauge (reuses the open lock).
        sqlite3_stmt* sz = nullptr;
        if (sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM operation_log", -1, &sz, nullptr) == SQLITE_OK) {
            if (sqlite3_step(sz) == SQLITE_ROW) final_rows = sqlite3_column_int64(sz, 0);
            sqlite3_finalize(sz);
        }
    }

    // §11 metrics emitted outside mu_ (the metric recorder has its own locking).
    OplogMetrics& m = OplogMetrics::Instance();
    m.RecordCleanupDeleted(OplogMetrics::CleanupReason::kAge, deleted_age);
    m.RecordCleanupDeleted(OplogMetrics::CleanupReason::kQuota, deleted_quota);
    m.ObserveCleanupDuration(static_cast<int>(NowMs() - cleanup_start_ms));
    if (failed) m.RecordCleanupFailed();
    if (final_rows >= 0) m.SetSizeRows(final_rows);
}

OperationLogStats OperationLogger::GetStats() {
    std::lock_guard<std::mutex> lock(mu_);
    OperationLogStats s;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_,
            "SELECT COUNT(*), COALESCE(MIN(timestamp),0), COALESCE(MAX(timestamp),0) "
            "FROM operation_log", -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            s.total_count = sqlite3_column_int64(stmt, 0);
            s.oldest_timestamp = sqlite3_column_int64(stmt, 1);
            s.latest_timestamp = sqlite3_column_int64(stmt, 2);
        }
        sqlite3_finalize(stmt);
    }
    // Approximate on-disk size: page_count * page_size of the global db.
    sqlite3_stmt* pg = nullptr;
    int64_t page_count = 0, page_size = 0;
    if (sqlite3_prepare_v2(db_, "PRAGMA page_count", -1, &pg, nullptr) == SQLITE_OK) {
        if (sqlite3_step(pg) == SQLITE_ROW) page_count = sqlite3_column_int64(pg, 0);
        sqlite3_finalize(pg);
    }
    if (sqlite3_prepare_v2(db_, "PRAGMA page_size", -1, &pg, nullptr) == SQLITE_OK) {
        if (sqlite3_step(pg) == SQLITE_ROW) page_size = sqlite3_column_int64(pg, 0);
        sqlite3_finalize(pg);
    }
    s.size_bytes = page_count * page_size;
    // §11 cortrix_oplog_size_rows: refresh the gauge from the live row count so it
    // stays current between cleanup sweeps (GetStats already counted the rows).
    OplogMetrics::Instance().SetSizeRows(s.total_count);
    return s;
}

HealthStatus OperationLogger::Health() {
    std::lock_guard<std::mutex> lock(mu_);
    HealthStatus h;
    h.last_error = last_error_;
    h.last_cleanup_timestamp = last_cleanup_timestamp_;
    // A trivial round-trip proves the handle is usable.
    h.db_path_reachable = false;
    sqlite3_stmt* stmt = nullptr;
    if (db_ != nullptr &&
        sqlite3_prepare_v2(db_, "SELECT 1", -1, &stmt, nullptr) == SQLITE_OK) {
        h.db_path_reachable = (sqlite3_step(stmt) == SQLITE_ROW);
        sqlite3_finalize(stmt);
    }
    h.is_healthy = h.db_path_reachable && last_error_.empty();
    return h;
}

int64_t OperationLogger::last_cleanup_deleted() const {
    std::lock_guard<std::mutex> lock(mu_);
    return last_cleanup_deleted_;
}

}  // namespace cortrix::observability
