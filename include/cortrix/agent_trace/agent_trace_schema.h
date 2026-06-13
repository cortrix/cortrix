#pragma once
#include <string>

#include "cortrix/catalog/schema_provider.h"

namespace cortrix::agent_trace {

/// Current agent_trace schema version (F13 §4.1). Phase 1 single-step create
/// (v0 → v1). Phase 2 internal evolution branches on (from, to) here.
constexpr int kAgentTraceSchemaVersion = 1;

/// The agent_trace DDL emitted by the F13 SchemaProvider (F13 §4.1 topic 3):
/// the agent_trace table + 3 indices. Stored in cortrix_global.db (the global
/// DB, NOT a per-namespace DB), but applied via the shared SchemaMigrator so it
/// runs inside the same versioned, atomic framework as the catalog / F18a
/// providers.
///
/// Open-Core (topic 7, §4.4): this is the CE-only schema. Ent's
/// agent_trace_extension table (input_tokens / output_tokens / query_pattern_id)
/// is a SEPARATE downstream migration keyed by agent_trace.id — it
/// does NOT alter this table (GEN-OpenCore-Boundary). cortrix/ has no
/// conditional compilation and does not know that table exists.
///
/// SQLite dialect note (cortrix_global.db is SQLite WAL): the §4.1 spec spells
/// session_id/trace_id/agent_id as VARCHAR(128); SQLite is dynamically typed and
/// accepts VARCHAR(n) as a type name (TEXT affinity, no length enforcement), so
/// the declared type is kept verbatim for spec fidelity. created_at is Unix ms
/// (topic 3 — same clock as operation_log, for the trace_id correlation chain).
extern const char* const kAgentTraceSchemaSql;

/// F13's ISchemaProvider (frozen cortrix::catalog::ISchemaProvider, D2-pre-5):
/// owns the agent_trace table + 3 indices in cortrix_global.db. Registered with
/// the SchemaMigrator that targets the global DB. Migrate returns Status
/// (F-FREEZE-1: no Result<void>).
class AgentTraceSchemaProvider : public cortrix::catalog::ISchemaProvider {
public:
    std::string FeatureName() const override { return "F13"; }
    int CurrentVersion() const override { return kAgentTraceSchemaVersion; }

    /// Phase 1 (from_ver 0 → 1): create agent_trace + 3 indices. An
    /// already-current (1 → 1) call is a defensive no-op. Any other step is a
    /// version mismatch until Phase 2.
    Status Migrate(sqlite3* db, int from_ver, int to_ver) override;
};

}  // namespace cortrix::agent_trace
