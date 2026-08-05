#pragma once
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include "cortrix/common/i_global_config.h"
#include "cortrix/observability/operation_logger.h"

struct sqlite3;

namespace cortrix::observability {

/// CE implementation of IOperationLogger. Writes the operation_log
/// table in cortrix_global.db synchronously (topic 4: Log P95 < 1ms — one prepared
/// INSERT on a WAL db). Open-Core: this is the CE-only concrete; Ent's
/// A downstream extended logger is a SEPARATE IOperationLogger that
/// double-writes audit_log_extension — cortrix/ has no #ifdef and does not know it
/// exists.
///
/// The handle is BORROWED, already-opened-and-migrated by the DI container (the
/// OperationLogSchemaProvider runs at startup via the shared SchemaMigrator —
/// this class does not migrate). Retention/row-cap come from IGlobalConfig
/// (operation_log_retention_days / operation_log_max_rows), read live in Cleanup()
/// so a config OnChange takes effect on the next sweep.
///
/// Thread-safe: a single mutex guards all DB access (the global db handle is
/// shared across Engine threads that all call Log()). The ObservabilityContext
/// trace_id/session_id are read by the Engine instrumentation site (C1 ThreadLocal) and passed
/// in via the entry; this class does not touch thread-locals.
class OperationLogger : public IOperationLogger {
public:
    /// @param db      borrowed, already-migrated cortrix_global.db handle (not owned).
    /// @param config  global config (retention / row-cap source). Must outlive this.
    OperationLogger(sqlite3* db, std::shared_ptr<IGlobalConfig> config);
    ~OperationLogger() override = default;

    OperationLogger(const OperationLogger&) = delete;
    OperationLogger& operator=(const OperationLogger&) = delete;

    void Log(const OperationLogEntry& entry,
             const TraceContext* ctx = nullptr) override;
    void BatchLog(const std::vector<OperationLogEntry>& entries,
                  const TraceContext* ctx = nullptr) override;
    Result<OperationLogQueryResult> Query(
        const OperationLogFilter& filter,
        const TraceContext* ctx = nullptr) override;
    void Cleanup() override;
    OperationLogStats GetStats() override;
    HealthStatus Health() override;

    /// Last delete count from Cleanup() (test/metric aid; the §11 metric wiring
    /// lands with OBS_SPEC). Reset at the start of each Cleanup().
    int64_t last_cleanup_deleted() const;

private:
    /// Insert one entry on `db_` (caller holds mu_). Returns false on a step
    /// failure; on success fills nothing (id is DB-assigned). `ctx` supplies a
    /// fallback trace/session when the entry leaves them unset.
    bool InsertLocked(const OperationLogEntry& entry, const TraceContext* ctx);

    sqlite3* db_;                                ///< borrowed (not owned)
    std::shared_ptr<IGlobalConfig> config_;
    mutable std::mutex mu_;

    // Health/stats bookkeeping (guarded by mu_).
    std::string last_error_;
    int64_t last_cleanup_timestamp_ = 0;
    int64_t last_cleanup_deleted_ = 0;
};

}  // namespace cortrix::observability
