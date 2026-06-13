#pragma once
#include <string>

#include "cortrix/catalog/schema_provider.h"

namespace cortrix::reranker {

/// F02's schema-migration contribution (F02 §2.5 / topic 2.3). The reranker's
/// per-namespace configuration lives in catalog.db `namespaces.reranker_config
/// JSONB`, which is one of the 11 standardized *_config columns already created by
/// the F12 base schema (catalog_schema.cpp). F02 therefore owns NO extension
/// column — the V1 keys (enabled / candidate_multiplier / max_candidates) are
/// JSON *values* inside that column, not SQL columns (topic 2.2/2.3).
///
/// Phase 1 Migrate(0 → 1) is consequently a no-op: F02 registers with the F12
/// SchemaMigrator purely so future reranker_config evolution runs inside the same
/// versioned, atomic framework as the other Feature providers (mirrors the
/// F21SchemaProvider / F09SchemaProvider pattern, ARCH §1.3.bis.3 topological
/// order). Phase 2 (§2.4 NS JSONB extension) activates the `model` /
/// `score_threshold` keys as per-NS overrides; those are JSON values inside the
/// existing column (not new SQL columns), so the V1 → V2 step bumps
/// CurrentVersion() without an ADD COLUMN.
///
/// Implements the frozen cortrix::catalog::ISchemaProvider (D2-pre-5). Migrate
/// returns Status, not Result<void> (F-FREEZE-1 / CODING_CONVENTIONS §3).
///
/// Standalone (D3): registering this with the live CatalogDb::Open(extra_providers)
/// list at server bootstrap is cross-Feature wiring → D3.5; here it is fully
/// unit-testable against a SchemaMigrator + the frozen F12 base schema.
class F02SchemaProvider : public cortrix::catalog::ISchemaProvider {
public:
    /// F12 registration key (aligns with the F21/F09 SchemaProvider naming).
    std::string FeatureName() const override { return "F02"; }

    /// Schema version. V1 = base reranker_config already in the F12 schema.
    /// Phase 2 -> V2 (§2.4 NS JSONB extension).
    int CurrentVersion() const override { return 1; }

    /// Phase 1 (from_ver 0 → 1): no-op (reranker_config supplied by the F12 base
    /// schema; F02 owns no extra column). An already-current (1 → 1) call is
    /// accepted defensively. Any other (from, to) is a version mismatch until
    /// Phase 2 implements the reranker_config V1 → V2 evolution.
    Status Migrate(sqlite3* db, int from_ver, int to_ver) override;
};

}  // namespace cortrix::reranker
