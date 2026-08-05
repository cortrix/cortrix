#include "cortrix/observability/observability_module.h"

#include <utility>

namespace cortrix::observability {

ObservabilityModule::ObservabilityModule(sqlite3* global_db,
                                         std::shared_ptr<IGlobalConfig> config)
    : global_db_(global_db), config_(std::move(config)) {}

ObservabilityModule::~ObservabilityModule() { Shutdown(); }

std::shared_ptr<IOperationLogger> ObservabilityModule::MakeLogger() {
    // CE: the open-source OperationLogger. Ent overrides this to return an
    // an extended logger (topic 10) — this tree never names that type.
    return std::make_shared<OperationLogger>(global_db_, config_);
}

void ObservabilityModule::Initialize() {
    if (initialized_) return;
    initialized_ = true;

    logger_ = MakeLogger();

    // Register operation_log for the daily UTC-02:00 sweep. The trace module registers
    // agent_trace / interaction_log on the same scheduler at its D3.
    auto logger = logger_;
    scheduler_.RegisterTable("operation_log", [logger] { logger->Cleanup(); });
    scheduler_.StartScheduler();

    // Inject the logger into every instrumentation site queued so far (§5.3).
    for (IOperationLoggerAware* aware : emitters_) {
        if (aware != nullptr) aware->SetOperationLogger(logger_);
    }
}

void ObservabilityModule::RegisterEmitter(IOperationLoggerAware* aware) {
    if (aware == nullptr) return;
    emitters_.push_back(aware);
    // If the logger already exists (registered after Initialize), inject now.
    if (logger_) aware->SetOperationLogger(logger_);
}

void ObservabilityModule::Shutdown() {
    scheduler_.StopScheduler();
}

}  // namespace cortrix::observability
