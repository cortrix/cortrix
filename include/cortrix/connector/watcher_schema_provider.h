#pragma once
#include <string>

#include "cortrix/catalog/schema_provider.h"

namespace cortrix::connector {

/// The watcher's schema-migration contribution. Its per-namespace
/// configuration lives in catalog.db `namespaces.watcher_config JSONB`, which is
/// one of the 11 standardized *_config columns already created by the catalog base
/// schema (catalog_schema.cpp). It therefore owns NO extension column and adds
/// NO per-Unit schema — watcher metadata is catalog-global (no Unit-owned index).
///
/// Phase 1 Migrate(0 → 1) is consequently a no-op: it registers with the
/// SchemaMigrator purely so that future watcher_config evolution runs inside the
/// same versioned, atomic framework as the other providers (catalog / parser /
/// block header / write coordinator / enricher / HyPE / doc summary / memory). Phase 2 activates
/// the 4 placeholder JSONB keys (file_patterns / ignore_patterns / auto_delete /
/// allowed_extensions) as per-NS overrides; those are JSON *values* inside the
/// existing column (not new SQL columns), so the V1 → V2 step bumps
/// CurrentVersion() without an ADD COLUMN.
///
/// Implements the frozen cortrix::catalog::ISchemaProvider (D2-pre-5). Migrate
/// returns Status, not Result<void> (F-FREEZE-1 / CODING_CONVENTIONS § 3) — the
/// § 2.6 sketch's `Result<void>` is reconciled to the frozen interface signature.
class WatcherSchemaProvider : public cortrix::catalog::ISchemaProvider {
public:
    /// Registration key (aligns with the startup registration order + the
    /// ParserSchemaProvider naming pattern).
    std::string FeatureName() const override { return "watcher"; }

    /// Schema version (bumped +1 on evolution; the SchemaMigrator does the
    /// idempotent per-version migration). V1 = base watcher_config already in
    /// the catalog schema. Phase 2 -> V2.
    int CurrentVersion() const override { return 1; }

    /// Phase 1 (from_ver 0 → 1): no-op (watcher_config is supplied by the catalog
    /// base schema; the watcher owns no extra column). An already-current (1 → 1) call
    /// is accepted defensively. Any other (from, to) is a version mismatch until
    /// Phase 2 implements the watcher_config V1 → V2 evolution.
    Status Migrate(sqlite3* db, int from_ver, int to_ver) override;
};

}  // namespace cortrix::connector
