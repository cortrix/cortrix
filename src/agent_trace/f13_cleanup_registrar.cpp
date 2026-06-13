#include "cortrix/agent_trace/f13_cleanup_registrar.h"

#include <sqlite3.h>

#include <chrono>
#include <cstdio>
#include <ctime>
#include <utility>

namespace cortrix::agent_trace {

namespace {

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

}  // namespace

std::string F13CleanupRegistrar::FormatIso8601Utc(int64_t unix_ms) {
    const std::time_t secs = static_cast<std::time_t>(unix_ms / 1000);
    const int millis = static_cast<int>(unix_ms % 1000);
    std::tm tm_utc{};
#if defined(_WIN32)
    gmtime_s(&tm_utc, &secs);
#else
    gmtime_r(&secs, &tm_utc);
#endif
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                  tm_utc.tm_year + 1900, tm_utc.tm_mon + 1, tm_utc.tm_mday,
                  tm_utc.tm_hour, tm_utc.tm_min, tm_utc.tm_sec, millis);
    return std::string(buf);
}

F13CleanupRegistrar::F13CleanupRegistrar(std::shared_ptr<IAgentTraceWriter> writer,
                                         sqlite3* db,
                                         std::shared_ptr<IGlobalConfig> config)
    : writer_(std::move(writer)), db_(db), config_(std::move(config)) {}

int F13CleanupRegistrar::CleanupInteractionLog() {
    if (db_ == nullptr) return 0;
    const int retention_days = config_ ? config_->interaction_log_retention_days : 180;
    const int64_t cutoff_ms = NowMs() - static_cast<int64_t>(retention_days) * 86400000LL;
    const std::string cutoff_iso = FormatIso8601Utc(cutoff_ms);

    // The real interaction_log.created_at is ISO-8601 TEXT (MEM01). ISO-8601 UTC
    // sorts lexicographically, so a string `<` comparison is a correct age filter.
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "DELETE FROM interaction_log WHERE created_at < ?",
                           -1, &stmt, nullptr) != SQLITE_OK) {
        return 0;
    }
    sqlite3_bind_text(stmt, 1, cutoff_iso.c_str(), -1, SQLITE_TRANSIENT);
    int deleted = 0;
    if (sqlite3_step(stmt) == SQLITE_DONE) deleted = sqlite3_changes(db_);
    sqlite3_finalize(stmt);
    return deleted;
}

void F13CleanupRegistrar::Register(observability::CleanupScheduler& scheduler) {
    // agent_trace 90d — delegate to the writer (it owns the DELETE + retention read).
    auto writer = writer_;
    scheduler.RegisterTable("agent_trace", [writer] {
        if (writer) writer->Cleanup();
    });

    // interaction_log 180d — F13 §10.1 / §11. interaction_sources cascades via FK.
    scheduler.RegisterTable("interaction_log", [this] { CleanupInteractionLog(); });
}

}  // namespace cortrix::agent_trace
