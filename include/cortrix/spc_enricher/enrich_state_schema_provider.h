#pragma once
#include <string>

#include "cortrix/catalog/schema_provider.h"

namespace cortrix::spc {

/// Per-Unit `enrich_state` sidecar table (addendum §3.7 Fork-2 = 2c): one row per
/// child block recording the chunk-level enrichment outcome (F03/F35/F38 chain).
/// The table is the durable SoT for enrichment coverage — the write phase inserts
/// a row for EVERY enriched-eligible child ('ok' or 'pending_retry'), the retry
/// sweeper scans due 'pending_retry' rows, and the backfill worker flips them.
/// Additive DDL only (no blocks-table changes); runs via SchemaMigrator::MigrateUnit
/// at F05 LoadOneNamespace, after the F09 framework provider.
class EnrichStateSchemaProvider : public cortrix::catalog::ISchemaProvider {
public:
    std::string FeatureName() const override { return "EnrichState"; }
    int CurrentVersion() const override { return 1; }

    /// 0 → 1: create enrich_state + its scan indexes. Idempotent
    /// (CREATE TABLE/INDEX IF NOT EXISTS); no dependency on other tables.
    Status Migrate(sqlite3* db, int from_ver, int to_ver) override;
};

}  // namespace cortrix::spc
