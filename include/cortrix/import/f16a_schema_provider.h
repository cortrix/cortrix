#pragma once
#include <string>

#include "cortrix/catalog/schema_provider.h"
#include "cortrix/common/status.h"

namespace cortrix::import {

/// Current F16a schema version (F16a §4.1 / §4.2). Phase 1 single-step create
/// (v0 → v1); Phase 2 internal evolution branches on (from, to).
constexpr int kF16aSchemaVersion = 1;

/// The F16a DDL emitted by the schema provider (F16a §4.1 db_connections +
/// §4.2 import_tasks). Lives in the catalog DB (catalog.db) alongside the
/// tenants / namespaces tables its FKs reference, applied via the shared F12
/// SchemaMigrator so it runs inside the same versioned, atomic framework.
///
/// Dialect note: the F16a §4.x spec is written in Postgres syntax (BIGSERIAL /
/// TIMESTAMP / now()); the catalog DB is SQLite, so this is transcribed to the
/// SQLite idiom every other provider uses (operation_log_schema.cpp /
/// catalog_schema.cpp): INTEGER PRIMARY KEY AUTOINCREMENT, Unix-ms INTEGER
/// timestamps, JSONB → TEXT affinity, IF NOT EXISTS on every object. Semantics
/// (columns / FKs / indices / 30d expiry) are 1:1 with the spec.
extern const char* const kF16aSchemaSql;

/// F16a's ISchemaProvider (frozen cortrix::catalog::ISchemaProvider, D2-pre-5):
/// owns db_connections + import_tasks + their indices in the catalog DB. Registered
/// with the catalog SchemaMigrator (after F12, so the tenants/namespaces FK targets
/// exist). Migrate returns Status (F-FREEZE-1: no Result<void>).
class F16aSchemaProvider : public cortrix::catalog::ISchemaProvider {
public:
    std::string FeatureName() const override { return "F16a"; }
    int CurrentVersion() const override { return kF16aSchemaVersion; }

    /// Phase 1 (from_ver 0 → 1): create db_connections + import_tasks + indices. An
    /// already-current (1 → 1) call is a defensive no-op. Any other step is a
    /// version mismatch until Phase 2.
    Status Migrate(sqlite3* db, int from_ver, int to_ver) override;
};

}  // namespace cortrix::import
