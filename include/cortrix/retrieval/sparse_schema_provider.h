#pragma once
#include <string>

#include "cortrix/catalog/schema_provider.h"

namespace cortrix::retrieval {

/// Canonical inverted-index DDL — the single SoT for the
/// sparse_inverted_index table, shared by SparseSchemaProvider::Migrate (the D3.5
/// integrated path) and SpladeSparseRetriever::Open (the standalone self-contained
/// path) so the two can never drift.
///
/// Notes:
///   - child_id is a LOGICAL FK to blocks.child_id (A unified-blocks): child rows
///     live in `blocks` and blocks.child_id has a partial-unique index
///     (WHERE child_id IS NOT NULL), which SQLite cannot bind as a hard FK parent
///     key — so the reference is app-enforced (join: blocks WHERE child_id=?).
///     ARCH §1.8.2 keeps sparse keyed by child_id (not hashed to block_id).
///   - the §4.3 inline `INDEX idx_term_lookup` is a separate CREATE INDEX
///     (SQLite has no inline table index).
inline constexpr const char* kSparseInvertedIndexDdl = R"SQL(
CREATE TABLE IF NOT EXISTS sparse_inverted_index (
    ns_id    TEXT NOT NULL,
    term_id  INTEGER NOT NULL,
    child_id TEXT NOT NULL,
    weight   REAL NOT NULL,
    PRIMARY KEY (ns_id, term_id, child_id)
);
CREATE INDEX IF NOT EXISTS idx_term_lookup
    ON sparse_inverted_index (ns_id, term_id, weight DESC);
CREATE INDEX IF NOT EXISTS idx_child_lookup
    ON sparse_inverted_index (ns_id, child_id);
)SQL";

/// The sparse-retrieval schema-migration contribution. Owns:
///   - the net-new `sparse_inverted_index` table (per-NS SPLADE postings), and
///   - the `blocks.sparse_vec` BLOB column (A unified-blocks: sparse_vec lives on
///     child rows of `blocks`, ALTER'd here — replaces the legacy children.sparse_vec
///     that was declared in store/parent_chunk_schema.h pre-A).
/// Unlike the reranker (which owns no extra column), this Migrate(0→1) emits real DDL.
///
/// Implements the frozen cortrix::catalog::ISchemaProvider (D2-pre-5); Migrate
/// returns Status (F-FREEZE-1 / CODING_CONVENTIONS §3), runs inside the
/// SchemaMigrator's transaction.
///
/// Standalone (D3): registering this with the live CatalogDb/Unit migrator at
/// bootstrap is cross-Feature wiring → D3.5. SpladeSparseRetriever::Open()
/// creates the same table self-contained for standalone tests; this provider is
/// the integrated, versioned path that runs against the Unit DB once wired.
class SparseSchemaProvider : public cortrix::catalog::ISchemaProvider {
public:
    /// Registration key (aligns with the other SchemaProvider names).
    std::string FeatureName() const override { return "sparse_index"; }

    /// V1 = the sparse_inverted_index table + blocks.sparse_vec column. Phase 2
    /// (IVF-sparse / sharding, §14) would bump this.
    int CurrentVersion() const override { return 1; }

    /// from_ver 0 → 1: CREATE the sparse_inverted_index table + indexes AND ALTER
    /// blocks ADD sparse_vec (idempotent; no-op if blocks absent). An already-current
    /// (1 → 1) call is accepted defensively. Any other (from, to) is a version
    /// mismatch until a Phase-2 step is defined.
    Status Migrate(sqlite3* db, int from_ver, int to_ver) override;
};

}  // namespace cortrix::retrieval
