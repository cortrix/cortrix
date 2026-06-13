#pragma once
#include <memory>
#include <vector>

#include "cortrix/common/i_global_config.h"
#include "cortrix/observability/cleanup_scheduler.h"
#include "cortrix/observability/operation_log_emitter.h"
#include "cortrix/observability/operation_logger.h"
#include "cortrix/observability/operation_logger_impl.h"

struct sqlite3;

namespace cortrix::observability {

/// CE dependency-injection module for F18a (§5.3, topic 10). Owns the CE
/// OperationLogger + the shared CleanupScheduler, registers operation_log for the
/// daily UTC-02:00 sweep, and injects the IOperationLogger into the 4 Engine instrumentation
/// sites + the API handler.
///
/// Open-Core (topic 10 / GEN-OpenCore-Boundary): the CE build constructs an
/// OperationLogger here. An embedder module MAY override MakeLogger() to
/// construct an extended logger instead — the rest of the wiring (scheduler,
/// instrumentation injection) is shared, and this tree never names any
/// override (interface-extension isolation).
///
/// Standalone (D3): the instrumentation sites are injected through the IOperationLoggerAware
/// seam; the REAL QueryEngine / SpcPipeline / MemoryStore / NamespaceManager
/// instances are attached at D3.5. `db` is the already-migrated cortrix_global.db
/// handle (the F18a OperationLogSchemaProvider ran at startup).
class ObservabilityModule {
public:
    ObservabilityModule(sqlite3* global_db, std::shared_ptr<IGlobalConfig> config);
    virtual ~ObservabilityModule();

    /// Build the logger (CE: OperationLogger). Virtual so the Ent module overrides
    /// it with an extended logger (topic 10). Called once by Initialize().
    virtual std::shared_ptr<IOperationLogger> MakeLogger();

    /// Construct the logger, register operation_log with the scheduler, and start
    /// the daily sweep. Idempotent.
    void Initialize();

    /// Register an instrumentation-bearing Engine module to receive the logger (§5.3). Call
    /// before Initialize() (queued) or after (injected immediately). The pointer
    /// is borrowed; the caller owns the module's lifetime (D3.5 attaches the real
    /// Engine instances).
    void RegisterEmitter(IOperationLoggerAware* aware);

    /// The injected logger (null before Initialize()).
    std::shared_ptr<IOperationLogger> logger() const { return logger_; }

    /// The shared cleanup scheduler (F13 registers its tables here too).
    CleanupScheduler& scheduler() { return scheduler_; }

    /// Stop the scheduler (also done by the destructor).
    void Shutdown();

protected:
    sqlite3* global_db_;
    std::shared_ptr<IGlobalConfig> config_;

private:
    std::shared_ptr<IOperationLogger> logger_;
    CleanupScheduler scheduler_;
    std::vector<IOperationLoggerAware*> emitters_;
    bool initialized_ = false;
};

}  // namespace cortrix::observability
