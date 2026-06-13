#pragma once
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include "cortrix/agent_trace/agent_trace_writer.h"
#include "cortrix/common/i_global_config.h"

struct sqlite3;

namespace cortrix::agent_trace {

/// CE implementation of IAgentTraceWriter (F13 §5.2). Writes the agent_trace table
/// in cortrix_global.db synchronously (one prepared INSERT on a WAL db). Open-Core:
/// this is the stock concrete; a downstream extension writer
/// is a SEPARATE IAgentTraceWriter that double-writes agent_trace_extension —
/// cortrix/ has no #ifdef and does not know it exists.
///
/// The handle is BORROWED, already-opened-and-migrated by the DI container (the
/// F13 AgentTraceSchemaProvider runs at startup via the shared SchemaMigrator — this
/// class does not migrate). Retention comes from IGlobalConfig
/// (agent_trace_retention_days), read live in Cleanup() so a config OnChange takes
/// effect on the next sweep.
///
/// Thread-safe: a single mutex guards all DB access (the global db handle is shared
/// across Engine threads that all call Write()). Write() is no-throw (C4): a write
/// failure records last_error_ but never propagates — observability must not break
/// the business path.
class AgentTraceWriterImpl : public IAgentTraceWriter {
public:
    /// @param db      borrowed, already-migrated cortrix_global.db handle (not owned).
    /// @param config  global config (retention source). Must outlive this.
    AgentTraceWriterImpl(sqlite3* db, std::shared_ptr<IGlobalConfig> config);
    ~AgentTraceWriterImpl() override = default;

    AgentTraceWriterImpl(const AgentTraceWriterImpl&) = delete;
    AgentTraceWriterImpl& operator=(const AgentTraceWriterImpl&) = delete;

    void Write(const AgentTraceEntry& entry,
               const observability::TraceContext* ctx = nullptr) override;
    Result<TraceSession> Query(const std::string& session_id,
                               const TraceFilter& filter,
                               const observability::TraceContext* ctx = nullptr) override;
    void OnSessionEnd(const std::string& session_id,
                      const observability::TraceContext* ctx = nullptr) override;
    void CheckAndMarkTimeoutSessions(
        int idle_threshold_seconds,
        const observability::TraceContext* ctx = nullptr) override;
    void Cleanup() override;

    /// Last delete count from Cleanup() (test/metric aid). Reset at each Cleanup().
    int64_t last_cleanup_deleted() const;
    /// Last DB error string (test/health aid).
    std::string last_error() const;

private:
    /// Insert one entry on `db_` (caller holds mu_). Returns false on a step
    /// failure. `ctx` supplies a fallback trace_id when the entry leaves it unset.
    bool InsertLocked(const AgentTraceEntry& entry, const observability::TraceContext* ctx);

    sqlite3* db_;                                ///< borrowed (not owned)
    std::shared_ptr<IGlobalConfig> config_;
    mutable std::mutex mu_;

    std::string last_error_;
    int64_t last_cleanup_timestamp_ = 0;
    int64_t last_cleanup_deleted_ = 0;
};

}  // namespace cortrix::agent_trace
