#pragma once
#include <string>

#include "cortrix/catalog/schema_provider.h"

namespace cortrix::spc {

/// The HyPE index schema-migration contribution. HyPE introduces the
/// kBlockHypeQuestion=16 block sub-type (common/block_types.h) but owns NO new
/// table and NO new column: the per-Unit `blocks` table (block-header-owned) already
/// stores an arbitrary block_type INTEGER, so a hype_question Block is just a row
/// with block_type=16. Its Phase-1 Migrate(0→1) is therefore a no-op — it
/// registers with the SchemaMigrator purely so any future hype_question
/// schema evolution runs inside the same versioned, atomic framework as the other
/// providers (mirrors the RerankerSchemaProvider no-op pattern, contrast the enricher which owns
/// real per-Unit columns).
///
/// Implements the frozen cortrix::catalog::ISchemaProvider. Migrate
/// returns Status (the HyPE design
/// wrote Result<void>, reconciled to the frozen Status signature here).
///
/// Standalone: registering this with the live SchemaMigrator at server
/// bootstrap (the MigrateUnit path) is wired separately; here it is
/// fully unit-testable against a SchemaMigrator.
class HypeSchemaProvider : public cortrix::catalog::ISchemaProvider {
public:
    /// Registration key (aligns with the other SchemaProvider names).
    std::string FeatureName() const override { return "hype"; }

    /// Schema version. V1 = the kBlockHypeQuestion=16 block sub-type (code-level
    /// enum; no DB schema object). Phase 2 (independent P-HNSW / versioning)
    /// would bump this.
    int CurrentVersion() const override { return 1; }

    /// Phase 1 (from_ver 0 → 1): no-op (hype_question Blocks reuse the existing blocks
    /// table's existing block_type column; HyPE owns no extra table/column). An
    /// already-current (n → n) call is accepted defensively. Any other step is a
    /// version mismatch (CX_ERR_HYPE_SCHEMA_VERSION_MISMATCH) until a Phase-2 step
    /// is defined.
    Status Migrate(sqlite3* db, int from_ver, int to_ver) override;
};

}  // namespace cortrix::spc
