#pragma once
#include <memory>
#include <string>

#include "cortrix/agent_trace/agent_trace_writer.h"
#include "cortrix/common/i_global_config.h"
#include "cortrix/observability/cleanup_scheduler.h"

struct sqlite3;

namespace cortrix::agent_trace {

/// Registers F13's two retention-cleanup tables onto the shared F18a
/// CleanupScheduler (F13 §10.1, S10 + S11 — reuses the F18a framework, no separate
/// timer):
///   - agent_trace  : 90 days (IGlobalConfig.agent_trace_retention_days), delegated
///                    to writer->Cleanup() (the writer owns the agent_trace DELETE).
///   - interaction_log : 180 days (IGlobalConfig.interaction_log_retention_days),
///                    a DELETE on the MVP interaction_log table. The real frozen
///                    created_at column is ISO-8601 TEXT (MEM01), so the cutoff is
///                    formatted as an ISO-8601 string and compared lexically (ISO
///                    8601 UTC sorts lexicographically). interaction_sources rows
///                    cascade via the FK (ON DELETE CASCADE).
///
/// Standalone: the daily UTC-02:00 wall-clock loop is the scheduler's (real); this
/// just registers the callbacks. Tests drive scheduler.RunCleanupNow().
class F13CleanupRegistrar {
public:
    /// @param writer  agent_trace writer (its Cleanup() is the agent_trace callback).
    /// @param db      borrowed handle holding interaction_log (for that callback). Not owned.
    /// @param config  retention source (read live each sweep).
    F13CleanupRegistrar(std::shared_ptr<IAgentTraceWriter> writer,
                        sqlite3* db,
                        std::shared_ptr<IGlobalConfig> config);

    /// Register both tables on `scheduler`. Idempotent registration is the caller's
    /// responsibility (call once at startup), mirroring F18a.
    void Register(observability::CleanupScheduler& scheduler);

    /// Delete interaction_log rows older than the retention window (exposed for the
    /// callback + tests). Computes the cutoff as an ISO-8601 string. Returns the
    /// number of rows deleted (best-effort; a DB error returns 0).
    int CleanupInteractionLog();

    /// Format `unix_ms` as an ISO-8601 UTC string "YYYY-MM-DDTHH:MM:SS.mmmZ"
    /// matching the interaction_log.created_at default. Exposed for tests.
    static std::string FormatIso8601Utc(int64_t unix_ms);

private:
    std::shared_ptr<IAgentTraceWriter> writer_;
    sqlite3* db_;  ///< borrowed (not owned)
    std::shared_ptr<IGlobalConfig> config_;
};

}  // namespace cortrix::agent_trace
