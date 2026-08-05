#pragma once
#include <string>

#include "cortrix/catalog/schema_provider.h"

namespace cortrix::spc {

/// The enricher's schema-migration contribution (registered
/// through the SchemaMigrator framework, aligning with the catalog
/// layered shared governance). Unlike the reranker and parser (which own no per-Unit schema), it owns a REAL
/// per-Unit migration: it extends the F09-owned per-Unit `blocks` table with 3
/// enrichment columns and adds its own `entities` table + FTS5 index.
///
/// Per-Unit migration (topic 2.6 ruling A 3-layer):
///   - blocks +3 cols: enriched_score (REAL, indexed) / enriched_at (INTEGER) /
///     enricher_metadata (TEXT JSONB)   — F03-owned extension columns on the
///     block-header framework table (the SoT stays there; the enricher owns these columns).
///   - entities table (entity_id PK, block_id FK, text, type, offsets) + indexes.
///   - entities_fts FTS5 (text, type; content='entities', content_rowid=entity_id).
///
/// Implements the frozen cortrix::catalog::ISchemaProvider. Migrate
/// returns Status (F-FREEZE-1 / CODING_CONVENTIONS §3 — not Result<void>). All
/// DDL is idempotent (table/column existence guards) so a re-run / already-current
/// Unit DB is a no-op; if `blocks` is absent (isolated unit test) it no-ops too.
///
/// Standalone (D3): registering this with the SchemaMigrator at server bootstrap
/// (the MigrateUnit path) is wired separately; here it is fully
/// unit-testable against a SchemaMigrator + an in-memory blocks table.
class F03SchemaProvider : public cortrix::catalog::ISchemaProvider {
public:
    /// Registration key (aligns with the other SchemaProvider names).
    std::string FeatureName() const override { return "enricher"; }

    /// Schema version. V1 = blocks +3 cols + entities + FTS5. V2 (R9 Tier C) =
    /// entities.block_id FK gains ON DELETE CASCADE so GC hard-delete / purge can
    /// reclaim blocks; bumped from 1 so existing units re-run Migrate and rebuild
    /// their pre-cascade entities table in place.
    int CurrentVersion() const override { return 2; }

    /// Forward, idempotent migration (0→1, 0→2, 1→2). The single pass applies the
    /// per-Unit DDL and, at v2, rebuilds any pre-cascade entities table. Backward
    /// or beyond-current steps are a version mismatch.
    Status Migrate(sqlite3* db, int from_ver, int to_ver) override;
};

}  // namespace cortrix::spc
