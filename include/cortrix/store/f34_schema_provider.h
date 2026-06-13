#pragma once
#include <string>

#include "cortrix/catalog/schema_provider.h"

namespace cortrix::store {

/// F34's per-Unit schema contribution (detailed design § 3.1, A unified-blocks
/// reconcile — Derek 2026-06-07). Registered through the F12 SchemaMigrator
/// framework (ARCH § 1.3.bis.3 provider #4, after F09 framework / before F03).
///
/// Under the A unified-blocks model, child chunks are rows of the F09-owned
/// per-Unit `blocks` table (child rows = `child_id IS NOT NULL`; block_type =
/// source modality, no kBlockChild enum). F34SchemaProvider owns:
///   - blocks +4 child columns: child_id (TEXT, business ULID + child-identity key)
///     / parent_id (TEXT, FK→parents) / token_count (INTEGER) / parent_offset
///     (INTEGER) — F34-owned extension columns on the F09 framework table.
///   - idx_blocks_child_id  (UNIQUE, WHERE child_id  IS NOT NULL) — child identity.
///   - idx_blocks_parent    (        WHERE parent_id IS NOT NULL) — parent expand.
///   - idx_blocks_meta_doc  (UNIQUE, WHERE block_type = 8/*META*/) — F08 "1 doc =
///     1 META" uniqueness (B2, Derek 2026-06-07 ruled this index is built by
///     F34SchemaProvider; its semantics belong to F08 — see F08 § 3.3).
///   - `parents` table (parent_text reverse-lookup store, § D6) via kParentsSchemaSql.
///
/// Implements the frozen cortrix::catalog::ISchemaProvider (F12 § 3.7). Migrate
/// returns Status (CODING_CONVENTIONS § 3). All DDL is idempotent (column/table
/// existence guards + IF NOT EXISTS) so a re-run / already-current Unit DB is a
/// no-op; if `blocks` is absent (isolated unit test) the blocks ALTERs no-op but
/// `parents` is still created.
class F34SchemaProvider : public cortrix::catalog::ISchemaProvider {
public:
    /// F12 registration key.
    std::string FeatureName() const override { return "F34"; }

    /// Schema version. V1 = blocks +4 child cols + 3 indexes + parents table.
    int CurrentVersion() const override { return 1; }

    /// Phase 1 (from_ver 0 → 1): apply the per-Unit DDL above (idempotent).
    Status Migrate(sqlite3* db, int from_ver, int to_ver) override;
};

}  // namespace cortrix::store
