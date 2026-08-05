#pragma once
#include <string>

#include "cortrix/catalog/schema_provider.h"

namespace cortrix::store {

/// The parent-child chunker's per-Unit schema contribution (unified-blocks
/// reconcile). Registered through the SchemaMigrator
/// framework (provider #4, after the block-header framework / before the enricher).
///
/// Under the A unified-blocks model, child chunks are rows of the F09-owned
/// per-Unit `blocks` table (child rows = `child_id IS NOT NULL`; block_type =
/// source modality, no kBlockChild enum). ParentChildSchemaProvider owns:
///   - blocks +4 child columns: child_id (TEXT, business ULID + child-identity key)
///     / parent_id (TEXT, FK→parents) / token_count (INTEGER) / parent_offset
///     (INTEGER) — chunker-owned extension columns on the framework table.
///   - idx_blocks_child_id  (UNIQUE, WHERE child_id  IS NOT NULL) — child identity.
///   - idx_blocks_parent    (        WHERE parent_id IS NOT NULL) — parent expand.
///   - idx_blocks_meta_doc  (UNIQUE, WHERE block_type = 8/*META*/) — the "1 doc =
///     1 META" uniqueness (this index is built by
///     ParentChildSchemaProvider; its semantics belong to the metadata block).
///   - `parents` table (parent_text reverse-lookup store, § D6) via kParentsSchemaSql.
///
/// Implements the frozen cortrix::catalog::ISchemaProvider. Migrate
/// returns Status (CODING_CONVENTIONS § 3). All DDL is idempotent (column/table
/// existence guards + IF NOT EXISTS) so a re-run / already-current Unit DB is a
/// no-op; if `blocks` is absent (isolated unit test) the blocks ALTERs no-op but
/// `parents` is still created.
class ParentChildSchemaProvider : public cortrix::catalog::ISchemaProvider {
public:
    /// Registration key.
    std::string FeatureName() const override { return "parent_child"; }

    /// Schema version. V1 = blocks +4 child cols + 3 indexes + parents table.
    int CurrentVersion() const override { return 1; }

    /// Phase 1 (from_ver 0 → 1): apply the per-Unit DDL above (idempotent).
    Status Migrate(sqlite3* db, int from_ver, int to_ver) override;
};

}  // namespace cortrix::store
