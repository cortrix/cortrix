#pragma once
#include <string>

#include "cortrix/catalog/schema_provider.h"
#include "cortrix/common/status.h"

namespace cortrix::import {

/// Current database-import schema version. Phase 1 single-step create
/// (v0 → v1); Phase 2 internal evolution branches on (from, to).
constexpr int kImportSchemaVersion = 1;

/// The DDL emitted by the schema provider (db_connections +
/// import_tasks). Lives in the catalog DB (catalog.db) alongside the
/// tenants / namespaces tables its FKs reference, applied via the shared
/// SchemaMigrator so it runs inside the same versioned, atomic framework.
///
/// Dialect note: the spec is written in Postgres syntax (BIGSERIAL /
/// TIMESTAMP / now()); the catalog DB is SQLite, so this is transcribed to the
/// SQLite idiom every other provider uses (operation_log_schema.cpp /
/// catalog_schema.cpp): INTEGER PRIMARY KEY AUTOINCREMENT, Unix-ms INTEGER
/// timestamps, JSONB → TEXT affinity, IF NOT EXISTS on every object. Semantics
/// (columns / FKs / indices / 30d expiry) are 1:1 with the spec.
extern const char* const kImportSchemaSql;

/// The database-import ISchemaProvider (frozen cortrix::catalog::ISchemaProvider):
/// owns db_connections + import_tasks + their indices in the catalog DB. Registered
/// with the catalog SchemaMigrator (after the catalog, so the tenants/namespaces FK targets
/// exist). Migrate returns Status (F-FREEZE-1: no Result<void>).
class ImportSchemaProvider : public cortrix::catalog::ISchemaProvider {
public:
    std::string FeatureName() const override { return "db_import"; }
    int CurrentVersion() const override { return kImportSchemaVersion; }

    /// Phase 1 (from_ver 0 → 1): create db_connections + import_tasks + indices. An
    /// already-current (1 → 1) call is a defensive no-op. Any other step is a
    /// version mismatch until Phase 2.
    Status Migrate(sqlite3* db, int from_ver, int to_ver) override;
};

}  // namespace cortrix::import
