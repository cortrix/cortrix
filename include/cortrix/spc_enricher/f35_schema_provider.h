#pragma once
#include <string>

#include "cortrix/catalog/schema_provider.h"

namespace cortrix::spc {

/// F35's per-Unit schema contribution (Contextual Retrieval, design § 4.1, A
/// unified-blocks reconcile). Registered through the F12
/// SchemaMigrator framework (ARCH § 1.3.bis.3 provider #6, after F03).
///
/// Under A unified-blocks, F35's columns live on child rows of the F09-owned
/// per-Unit `blocks` table (child rows = child_id IS NOT NULL), ALTER'd here
/// (was the children-table merge pre-A). F35SchemaProvider owns 4 blocks columns:
///   - embedding (BLOB)                — original child dense embedding (BGE-M3
///     1024-dim; F35-9 B dual-vector coexistence).
///   - contextualized_text (TEXT)      — LLM contextual prefix + chunk (F35-5 B).
///   - contextualized_embedding (BLOB) — BGE-M3 embedding of contextualized_text.
///   - contextualized_status (SMALLINT DEFAULT 0) — 0=pending 1=generated 2=failed
///     3=skipped_no_llm.
/// (chunk_index is NOT owned by F35 — it is an F09 framework blocks column.
/// metadata is in blocks.metadata_json, the shared framework JSONB column.)
///
/// V2 adds `contextual_vec_labels` (addendum §3.8 W2 wiring): the F35-9 B
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
    /// F12 registration key.
    std::string FeatureName() const override { return "F35"; }

    /// Schema version. V1 = blocks +4 contextual-retrieval columns.
    /// V2 = + contextual_vec_labels (dual-vector ANN label → child mapping).
    int CurrentVersion() const override { return 2; }

    /// Forward-only, idempotent: 0→1/0→2 create the columns; 1→2/0→2 create the
    /// label mapping table.
    Status Migrate(sqlite3* db, int from_ver, int to_ver) override;
};

}  // namespace cortrix::spc
