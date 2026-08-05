#pragma once
#include <string>

#include "cortrix/catalog/schema_provider.h"

namespace cortrix::spc {

/// The contextual-retrieval per-Unit schema contribution
/// (unified-blocks reconcile). Registered through the
/// SchemaMigrator framework (provider #6, after the enricher).
///
/// Under unified blocks, these columns live on child rows of the framework-owned
/// per-Unit `blocks` table (child rows = child_id IS NOT NULL), ALTER'd here
/// (was the children-table merge pre-A). F35SchemaProvider owns 4 blocks columns:
///   - embedding (BLOB)                — original child dense embedding (BGE-M3
///     1024-dim; dual-vector coexistence).
///   - contextualized_text (TEXT)      — LLM contextual prefix + chunk.
///   - contextualized_embedding (BLOB) — BGE-M3 embedding of contextualized_text.
///   - contextualized_status (SMALLINT DEFAULT 0) — 0=pending 1=generated 2=failed
///     3=skipped_no_llm.
/// (chunk_index is NOT owned here — it is a framework blocks column.
/// metadata is in blocks.metadata_json, the shared framework JSONB column.)
///
/// V2 adds `contextual_vec_labels`: the
/// dual-vector decision puts the contextualized embedding into P-HNSW as its OWN
/// point, under a derived label (HashChildIdToBlockId(child_id + ":ctx")). The
/// label is not a blocks row, so this mapping table is what lets the query side
/// resolve an ANN hit on a contextual point back to its child:
///   contextual_vec_labels(label PK, block_id, child_id)
///
/// Implements the frozen cortrix::catalog::ISchemaProvider. Migrate
/// returns Status (CODING_CONVENTIONS § 3). All DDL is idempotent (column
/// existence guards); if `blocks` is absent (isolated unit test) it no-ops.
class F35SchemaProvider : public cortrix::catalog::ISchemaProvider {
public:
    /// Registration key.
    std::string FeatureName() const override { return "contextual"; }

    /// Schema version. V1 = blocks +4 contextual-retrieval columns.
    /// V2 = + contextual_vec_labels (dual-vector ANN label → child mapping).
    int CurrentVersion() const override { return 2; }

    /// Forward-only, idempotent: 0→1/0→2 create the columns; 1→2/0→2 create the
    /// label mapping table.
    Status Migrate(sqlite3* db, int from_ver, int to_ver) override;
};

}  // namespace cortrix::spc
