#pragma once
#include <string>

#include "cortrix/catalog/schema_provider.h"

namespace cortrix::connector {

/// F21's schema-migration contribution. The watcher's per-namespace
/// configuration lives in catalog.db `namespaces.watcher_config JSONB`, which is
/// one of the 11 standardized *_config columns already created by the F12 base
/// schema (catalog_schema.cpp). F21 therefore owns NO extension column and adds
/// NO per-Unit schema — watcher metadata is catalog-global (no Unit-owned index).
///
/// Phase 1 Migrate(0 → 1) is consequently a no-op: it registers F21 with the F12
/// SchemaMigrator purely so that future watcher_config evolution runs inside the
/// same versioned, atomic framework as the other Feature providers (F12 / F06 /
/// F09 / F25 / F03 / F38 / F41 / MEM01). Phase 2 (TD-WATCHER-OPTIONS) activates
/// the 4 placeholder JSONB keys (file_patterns / ignore_patterns / auto_delete /
/// allowed_extensions) as per-NS overrides; those are JSON *values* inside the
/// existing column (not new SQL columns), so the V1 → V2 step bumps
/// CurrentVersion() without an ADD COLUMN.
///
/// Implements the frozen cortrix::catalog::ISchemaProvider (D2-pre-5). Migrate
/// returns Status, not Result<void> (F-FREEZE-1 / CODING_CONVENTIONS § 3) — the
/// § 2.6 sketch's `Result<void>` is reconciled to the frozen interface signature.
class F21SchemaProvider : public cortrix::catalog::ISchemaProvider {
public:
    /// F12 registration key (aligns with F12 § 5 startup registration + the
    /// F06SchemaProvider naming pattern).
    std::string FeatureName() const override { return "F21"; }

    /// Schema version (bumped +1 on evolution; F12 SchemaMigrator does the
    /// idempotent per-version migration). V1 = base watcher_config already in
    /// the F12 schema. Phase 2 -> V2 (TD-WATCHER-OPTIONS).
    int CurrentVersion() const override { return 1; }

    /// Phase 1 (from_ver 0 → 1): no-op (watcher_config is supplied by the F12
    /// base schema; F21 owns no extra column). An already-current (1 → 1) call
    /// is accepted defensively. Any other (from, to) is a version mismatch until
    /// Phase 2 implements the watcher_config V1 → V2 evolution.
    Status Migrate(sqlite3* db, int from_ver, int to_ver) override;
};

}  // namespace cortrix::connector
