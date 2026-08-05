#pragma once
#include <string>

#include "cortrix/catalog/schema_provider.h"

namespace cortrix::store {

/// The block-header schema-migration contribution. It owns the per-Unit
/// FRAMEWORK schema (documents / blocks / blocks_fts + indices + triggers): its
/// 0 → 1 migration creates them from kPerUnitFrameworkDdl (the single DDL SoT,
/// also reused by CortrixStoreSqlite::Open's standalone owns-db path). Runs via
/// SchemaMigrator::MigrateUnit at LoadOneNamespace (unified
/// schema governance). The Phase-2 header V1→V2 upgrade (TD-BLOCK-V2-UPGRADE)
/// will run inside the same versioned framework.
///
/// Implements the frozen cortrix::catalog::ISchemaProvider (D2-pre-5). Migrate
/// returns Status (F-FREEZE-1: no Result<void>) — the §4.4 sketch's Result<void>
/// is reconciled to the frozen interface signature.
class BlockFrameworkSchemaProvider : public cortrix::catalog::ISchemaProvider {
public:
    std::string FeatureName() const override { return "block_framework"; }
    int CurrentVersion() const override { return 1; }

    /// Phase 1 (from_ver 0 → 1): create the per-Unit framework schema from
    /// kPerUnitFrameworkDdl. Any other (from, to) is a version mismatch until
    /// Phase 2 implements the header V1→V2 migration.
    Status Migrate(sqlite3* db, int from_ver, int to_ver) override;
};

}  // namespace cortrix::store
