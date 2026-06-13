#pragma once
#include <string>

#include "cortrix/catalog/schema_provider.h"

namespace cortrix::spc {

/// F03's schema-migration contribution (design §3.1, v1.0.2 Major-2 — registered
/// through the F12 SchemaMigrator framework, aligning with F12 D1 topic 2.6 C
/// layered shared governance). Unlike F02/F06 (which own no per-Unit schema), F03 owns a REAL
/// per-Unit migration: it extends the F09-owned per-Unit `blocks` table with 3
/// enrichment columns and adds its own `entities` table + FTS5 index.
///
/// Per-Unit migration (topic 2.6 ruling A 3-layer):
///   - blocks +3 cols: enriched_score (REAL, indexed) / enriched_at (INTEGER) /
///     enricher_metadata (TEXT JSONB)   — F03-owned extension columns on the
///     F09 framework table (SoT stays F09; F03 owns these columns).
///   - entities table (entity_id PK, block_id FK, text, type, offsets) + indexes.
///   - entities_fts FTS5 (text, type; content='entities', content_rowid=entity_id).
///
/// Implements the frozen cortrix::catalog::ISchemaProvider (F12 §3.7). Migrate
/// returns Status (F-FREEZE-1 / CODING_CONVENTIONS §3 — not Result<void>). All
/// DDL is idempotent (table/column existence guards) so a re-run / already-current
/// Unit DB is a no-op; if `blocks` is absent (isolated unit test) it no-ops too.
///
/// Standalone (D3): registering this with the SchemaMigrator at server bootstrap
/// (the F12 MigrateUnit path) is cross-Feature wiring → D3.5; here it is fully
/// unit-testable against a SchemaMigrator + an in-memory blocks table.
class F03SchemaProvider : public cortrix::catalog::ISchemaProvider {
public:
    /// F12 registration key (aligns with F02/F06/F09 SchemaProvider naming).
    std::string FeatureName() const override { return "F03"; }

    /// Schema version. V1 = blocks +3 cols + entities + FTS5.
    int CurrentVersion() const override { return 1; }

    /// Phase 1 (from_ver 0 → 1): apply the per-Unit DDL above (idempotent).
    /// An already-current (n → n) call is accepted defensively. Any other step
    /// is a version mismatch until a future schema bump implements it.
    Status Migrate(sqlite3* db, int from_ver, int to_ver) override;
};

}  // namespace cortrix::spc
