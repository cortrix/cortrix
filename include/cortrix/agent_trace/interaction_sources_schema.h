#pragma once
#include <string>

#include "cortrix/catalog/schema_provider.h"

namespace cortrix::agent_trace {

/// Current interaction_sources schema version. Phase 1 single-step
/// create (v0 -> v1).
constexpr int kInteractionSourcesSchemaVersion = 1;

/// The interaction_sources DDL emitted by the observability SchemaProvider (
/// 6 + v1.0.5 §9.2 split). The CE citation-provenance table for GET
/// /interactions/{id}/sources (T-106): which blocks/memories a past interaction's
/// answer was grounded in.
///
/// Open-Core (v1.0.5 final): this is the CE table WITHOUT highlight ranges. The
/// Ent interaction_sources_extension table (highlight_start / highlight_end /
/// char_offset, T-107) is a SEPARATE downstream migration keyed by
/// interaction_sources.id -- it does NOT alter this table. cortrix/ has no
/// conditional compilation and does not know that table exists.
///
/// FK note: interaction_id references the MVP interaction_log table, whose real
/// frozen PK is `id TEXT` (UUID v4; see memory/interaction_log.h + the
/// CREATE TABLE in memory_store.cpp) -- NOT the `interaction_id INTEGER` the
/// earlier draft assumed (it predates the role/content rebuild). The
/// column is therefore TEXT to match the real PK; ON DELETE CASCADE keeps the
/// provenance rows in lock-step with the interaction. (Reported to the lead as the
/// same root cause as the S11 interaction_log finding; aligning a net-new table's
/// FK type to the real frozen PK is a stale-naming alignment, not a contract change.)
extern const char* const kInteractionSourcesSchemaSql;

/// The ISchemaProvider for interaction_sources (frozen cortrix::catalog::
/// ISchemaProvider, D2-pre-5). Lives in the same DB as interaction_log (the
/// memory/global DB). Registered with the SchemaMigrator. FeatureName is distinct
/// from the agent_trace provider's key so both can coexist in schema_version.
class InteractionSourcesSchemaProvider : public cortrix::catalog::ISchemaProvider {
public:
    std::string FeatureName() const override { return "F13_interaction_sources"; }
    int CurrentVersion() const override { return kInteractionSourcesSchemaVersion; }

    Status Migrate(sqlite3* db, int from_ver, int to_ver) override;
};

}  // namespace cortrix::agent_trace
