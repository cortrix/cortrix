#pragma once
#include <cstdint>
#include <memory>
#include <mutex>

#include "cortrix/common/i_global_config.h"
#include "cortrix/common/result.h"

namespace cortrix::catalog {
class INSRouter;
}
namespace cortrix::resource {
class INamespacePool;
}

namespace cortrix::agent_trace {

/// Per-NS interaction_log retention sweeper.
///
/// TC4 keeps interaction_log + interaction_sources PER-NAMESPACE (one table per
/// namespace memory.db, FK-tied to the interaction_log). The single-db
/// AgentTraceCleanupRegistrar reaches only one db, so the 180-day interaction_log cleanup
/// cannot be a single RegisterTable callback the way the global agent_trace one is.
///
/// This sweeper fills that gap exactly like DocumentGcSweeper does for the document
/// layer: it iterates namespaces (via INSRouter), acquires each NamespaceFacade (via
/// INamespacePool), and runs the interaction_log DELETE over that namespace's
/// memory.db. interaction_sources rows cascade via the FK (ON DELETE CASCADE).
///
/// Wiring: bootstrap registers RunOnce() on the shared CleanupScheduler under
/// the logical name "interaction_log" (one callback that fans out over all NS), so it
/// runs on the same daily UTC-02:00 sweep as agent_trace / operation_log. The
/// retention window is read live from IGlobalConfig (interaction_log_retention_days)
/// each sweep, so a config OnChange takes effect next cycle.
class InteractionLogSweeper {
public:
    /// @param ns_router  namespace enumerator (borrowed). Must outlive this.
    /// @param pool       NS pool used to acquire each memory.db façade (borrowed).
    /// @param config     retention source (read live each sweep).
    InteractionLogSweeper(catalog::INSRouter* ns_router,
                          resource::INamespacePool* pool,
                          std::shared_ptr<IGlobalConfig> config);

    InteractionLogSweeper(const InteractionLogSweeper&) = delete;
    InteractionLogSweeper& operator=(const InteractionLogSweeper&) = delete;

    struct SweepReport {
        int namespaces_swept = 0;        ///< namespaces whose memory.db was reached
        int namespaces_skipped = 0;      ///< namespaces gone / unloadable (skipped)
        int64_t interactions_deleted = 0;///< interaction_log rows removed across all NS
    };

    /// Delete interaction_log rows older than the retention window from every
    /// namespace's memory.db. A namespace that cannot be acquired is skipped (counted)
    /// rather than failing the whole sweep — observability cleanup must not wedge on
    /// one bad NS. Returns the aggregate report; the underlying enumeration error is
    /// propagated only when the namespace list itself cannot be read.
    Result<SweepReport> RunOnce();

private:
    catalog::INSRouter* ns_router_;        // borrowed
    resource::INamespacePool* pool_;       // borrowed
    std::shared_ptr<IGlobalConfig> config_;
    mutable std::mutex mu_;
};

}  // namespace cortrix::agent_trace
