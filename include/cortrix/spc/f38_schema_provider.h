#pragma once
#include <string>

#include "cortrix/catalog/schema_provider.h"

namespace cortrix::spc {

/// F38's schema-migration contribution. F38 introduces the
/// kBlockHypeQuestion=16 block sub-type (common/block_types.h) but owns NO new
/// table and NO new column: the per-Unit `blocks` table (F09-owned) already
/// stores an arbitrary block_type INTEGER, so a hype_question Block is just a row
/// with block_type=16. F38's Phase-1 Migrate(0→1) is therefore a no-op — it
/// registers with the F12 SchemaMigrator purely so any future hype_question
/// schema evolution runs inside the same versioned, atomic framework as the other
/// providers (mirrors the F02SchemaProvider no-op pattern, contrast F03 which owns
/// real per-Unit columns).
///
/// Implements the frozen cortrix::catalog::ISchemaProvider. Migrate
/// returns Status (F-FREEZE-1 / CODING_CONVENTIONS §3 — the F38 detailed design
/// §4.4 wrote Result<void>, reconciled to the frozen Status signature here).
///
/// Standalone (D3): registering this with the live SchemaMigrator at server
/// bootstrap (the F12 MigrateUnit path) is cross-Feature wiring → D3.5; here it is
/// fully unit-testable against a SchemaMigrator.
class F38SchemaProvider : public cortrix::catalog::ISchemaProvider {
public:
    /// F12 registration key (aligns with F02/F03/F09 SchemaProvider naming).
    std::string FeatureName() const override { return "F38"; }

    /// Schema version. V1 = the kBlockHypeQuestion=16 block sub-type (code-level
    /// enum; no DB schema object). Phase 2 (independent P-HNSW / versioning, §14)
    /// would bump this.
    int CurrentVersion() const override { return 1; }

    /// Phase 1 (from_ver 0 → 1): no-op (hype_question Blocks reuse the F09 blocks
    /// table's existing block_type column; F38 owns no extra table/column). An
    /// already-current (n → n) call is accepted defensively. Any other step is a
    /// version mismatch (CX_ERR_F38_SCHEMA_VERSION_MISMATCH) until a Phase-2 step
    /// is defined.
    Status Migrate(sqlite3* db, int from_ver, int to_ver) override;
};

}  // namespace cortrix::spc
