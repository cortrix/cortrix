#pragma once
#include <string>

#include "cortrix/catalog/schema_provider.h"
#include "cortrix/common/status.h"

namespace cortrix::scoring {

/// Current scoring schema version. Phase 1 single-step (v0 → v1) — add the blocks.semantic_score
/// column. Phase 2 (hotness_score coordination) may branch on (from, to).
constexpr int kScoringSchemaVersion = 1;

/// The scoring ISchemaProvider (frozen cortrix::catalog::ISchemaProvider): owns the
/// per-Unit `blocks.semantic_score` column (ARCH §5.1.2 — a separate per-Unit SQLite `blocks` column,
/// NOT in BlockHeader). Mirrors the enricher provider's idempotent ADD-COLUMN-if-absent
/// pattern (src/spc/enricher_schema_provider.cpp added enriched_score the same way). Registered
/// with the per-Unit SchemaMigrator. Migrate returns Status (F-FREEZE-1: no Result<void>).
///
/// Co-existence with the enricher: it adds `enriched_score` (the LLM enrichment score it writes itselfng),
/// scoring adds `semantic_score` (the write-time score, its primary responsibility — 2 independent columns). Both ALTER the same
/// blocks table idempotently; running order is insensitive (each guards on ColumnExists).
class ScoringSchemaProvider : public cortrix::catalog::ISchemaProvider {
public:
    std::string FeatureName() const override { return "scoring"; }
    int CurrentVersion() const override { return kScoringSchemaVersion; }

    /// Phase 1 (from_ver 0 → 1): ADD blocks.semantic_score (REAL, default NULL) iff absent
    /// + a partial index on scored rows. No-op if the blocks table is absent (isolated unit
    /// test not building the per-Unit framework). An already-current (1 → 1) call is a no-op.
    Status Migrate(sqlite3* db, int from_ver, int to_ver) override;
};

}  // namespace cortrix::scoring
