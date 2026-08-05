#pragma once
#include <cstdint>
#include <memory>
#include <string>

#include "cortrix/agent_trace/agent_trace_writer.h"
#include "cortrix/common/i_global_config.h"
#include "cortrix/observability/cleanup_scheduler.h"

struct sqlite3;

namespace cortrix::agent_trace {

/// Registers the observability retention-cleanup callbacks onto the shared CleanupScheduler
/// (reuses the operation-log framework, no separate timer).
///
/// The two retention tables live in different DBs, so this registrar covers ONLY
/// the single-db tables it can reach through `db_`:
///   - agent_trace  : 90 days (IGlobalConfig.agent_trace_retention_days), delegated
///                    to writer->Cleanup() (the writer owns the agent_trace DELETE).
///                    Lives in cortrix_global.db (TC4); the writer + this registrar
///                    are built over that handle.
///   - interaction_log : 180 days — TC4 keeps it PER-NS (one table per namespace
///                    memory.db), which a single-db registrar cannot reach. Its
///                    cleanup is the InteractionLogSweeper (per-NS), NOT this class.
///                    CleanupInteractionLog() below still operates on the one `db_`
///                    handle (used by the sweeper per NS + by tests); the
///                    Register*() surface no longer registers interaction_log here.
///
/// Standalone: the daily UTC-02:00 wall-clock loop is the scheduler's (real); this
/// just registers the callbacks. Tests drive scheduler.RunCleanupNow().
class F13CleanupRegistrar {
public:
    /// @param writer  agent_trace writer (its Cleanup() is the agent_trace callback).
    /// @param db      borrowed handle holding interaction_log (for CleanupInteractionLog,
    ///                used per-NS by the sweeper + by tests). Not owned. May be null
    ///                when only the agent_trace callback is needed (global wiring).
    /// @param config  retention source (read live each sweep).
    F13CleanupRegistrar(std::shared_ptr<IAgentTraceWriter> writer,
                        sqlite3* db,
                        std::shared_ptr<IGlobalConfig> config);

    /// Register the global agent_trace 90d cleanup on `scheduler` (TC4 — the only
    /// single-db table; interaction_log is per-NS, see InteractionLogSweeper).
    /// Idempotent registration is the caller's responsibility (call once at startup),
    /// mirroring the operation log.
    void RegisterAgentTrace(observability::CleanupScheduler& scheduler);

    /// Delete interaction_log rows older than the retention window from `db_` (exposed
    /// for the per-NS sweeper + tests). Computes the cutoff as an ISO-8601 string.
    /// Returns the number of rows deleted (best-effort; a DB error returns 0).
    int CleanupInteractionLog();

    /// Format `unix_ms` as an ISO-8601 UTC string "YYYY-MM-DDTHH:MM:SS.mmmZ"
    /// matching the interaction_log.created_at default. Exposed for tests + the sweeper.
    static std::string FormatIso8601Utc(int64_t unix_ms);

private:
    std::shared_ptr<IAgentTraceWriter> writer_;
    sqlite3* db_;  ///< borrowed (not owned)
    std::shared_ptr<IGlobalConfig> config_;
};

}  // namespace cortrix::agent_trace
