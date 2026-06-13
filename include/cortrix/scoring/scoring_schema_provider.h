#pragma once
#include <string>

#include "cortrix/catalog/schema_provider.h"
#include "cortrix/common/status.h"

namespace cortrix::scoring {

/// Current F07 schema version. Phase 1 single-step (v0 → v1) — add the blocks.semantic_score
/// column. Phase 2 (F43 hotness_score coordination) may branch on (from, to).
constexpr int kScoringSchemaVersion = 1;

/// F07's ISchemaProvider (frozen cortrix::catalog::ISchemaProvider, D2-pre-5): owns the
/// per-Unit `blocks.semantic_score` column (ARCH §5.1.2 — a separate per-Unit SQLite `blocks` column,
/// NOT in BlockHeader; F07 §3). Mirrors the F03 provider's idempotent ADD-COLUMN-if-absent
/// pattern (src/spc/f03_schema_provider.cpp added enriched_score the same way). Registered
/// with the per-Unit SchemaMigrator. Migrate returns Status (F-FREEZE-1: no Result<void>).
///
/// Co-existence with F03: F03 adds `enriched_score` (the LLM enrichment score, written by F03 itself, D8 ruling),
/// F07 adds `semantic_score` (the write-time score, F07's primary responsibility, D1 ruling — 2 independent columns). Both ALTER the same
/// blocks table idempotently; running order is insensitive (each guards on ColumnExists).
class ScoringSchemaProvider : public cortrix::catalog::ISchemaProvider {
public:
    std::string FeatureName() const override { return "F07"; }
    int CurrentVersion() const override { return kScoringSchemaVersion; }

    /// Phase 1 (from_ver 0 → 1): ADD blocks.semantic_score (REAL, default NULL) iff absent
    /// + a partial index on scored rows. No-op if the blocks table is absent (isolated unit
    /// test not building the per-Unit framework). An already-current (1 → 1) call is a no-op.
    Status Migrate(sqlite3* db, int from_ver, int to_ver) override;
};

}  // namespace cortrix::scoring
