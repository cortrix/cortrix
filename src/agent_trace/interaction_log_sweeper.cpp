#include "cortrix/agent_trace/interaction_log_sweeper.h"

#include <sqlite3.h>

#include <chrono>
#include <string>

#include "cortrix/agent_trace/f13_cleanup_registrar.h"  // FormatIso8601Utc (shared cutoff format)
#include "cortrix/catalog/i_ns_router.h"
#include "cortrix/resource/namespace_facade.h"

namespace cortrix::agent_trace {

namespace {

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// Delete interaction_log rows older than `cutoff_iso` from one namespace's memory.db.
// The real interaction_log.created_at is ISO-8601 TEXT; ISO-8601 UTC sorts
// lexicographically, so a string `<` comparison is a correct age filter (mirrors
// F13CleanupRegistrar::CleanupInteractionLog). interaction_sources cascades via the
// FK. Returns rows deleted (0 on a prepare/step error — best-effort per NS).
int64_t DeleteExpiredInteractionLog(sqlite3* mem_db, const std::string& cutoff_iso) {
    if (mem_db == nullptr) return 0;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(mem_db, "DELETE FROM interaction_log WHERE created_at < ?",
                           -1, &stmt, nullptr) != SQLITE_OK) {
        return 0;
    }
    sqlite3_bind_text(stmt, 1, cutoff_iso.c_str(), -1, SQLITE_TRANSIENT);
    int64_t deleted = 0;
    if (sqlite3_step(stmt) == SQLITE_DONE) deleted = sqlite3_changes(mem_db);
    sqlite3_finalize(stmt);
    return deleted;
}

}  // namespace

InteractionLogSweeper::InteractionLogSweeper(catalog::INSRouter* ns_router,
                                             resource::INamespacePool* pool,
                                             std::shared_ptr<IGlobalConfig> config)
    : ns_router_(ns_router), pool_(pool), config_(std::move(config)) {}

Result<InteractionLogSweeper::SweepReport> InteractionLogSweeper::RunOnce() {
    std::lock_guard<std::mutex> lock(mu_);
    SweepReport report;
    if (ns_router_ == nullptr || pool_ == nullptr) return report;  // not wired -> no-op

    const int retention_days = config_ ? config_->interaction_log_retention_days : 180;
    const int64_t cutoff_ms = NowMs() - static_cast<int64_t>(retention_days) * 86400000LL;
    const std::string cutoff_iso = F13CleanupRegistrar::FormatIso8601Utc(cutoff_ms);

    auto ns_list = ns_router_->ListNamespaces();
    if (!ns_list.ok()) return ns_list.status();  // can't enumerate -> propagate

    for (const auto& ns_id : ns_list.value().results) {
        resource::NamespaceFacade facade(*pool_, ns_id);
        if (!facade.Acquire().ok()) {  // NS gone / unloadable -> skip, don't fail sweep
            ++report.namespaces_skipped;
            continue;
        }
        sqlite3* mem_db = facade.memory().db_handle();
        report.interactions_deleted += DeleteExpiredInteractionLog(mem_db, cutoff_iso);
        ++report.namespaces_swept;
    }
    return report;
}

}  // namespace cortrix::agent_trace
