#pragma once
#include <string>

#include "cortrix/catalog/schema_provider.h"

namespace cortrix::catalog {

/// Current catalog schema version. Bumped only for Phase 1
/// internal schema evolution (>= 100 → 110 → ...); MVP → Phase 1 cross-version
/// migration is out of scope and goes through MVP_MIGRATION_POLICY.md clean break
/// migrations.
constexpr int kCatalogSchemaVersion = 2;  // +1: OPEN-2 GC blob_gc_queue SoT reshape (D3.5 r2)

/// The catalog.db DDL emitted by the catalog SchemaProvider: 6 main tables
/// (file_locations / ns_units / units / nodes / tenants / namespaces) + 6
/// association tables (content_refs / blob_gc_queue / cross_tenant_whitelist /
/// users / user_tenants / ns_acl), faithfully transcribed from
/// F12-two-layer-mapping.md §4.1 (which mirrors ARCHITECTURE.md §2.4).
///
/// Notes on the SQLite dialect (catalog.db is SQLite WAL):
///  - The spec SQL uses `JSONB` / `BIGINT` column types and `'{}'::jsonb`
///    defaults — these are PostgreSQL spellings. SQLite is dynamically typed
///    and accepts `JSONB`/`BIGINT` as type names (affinity rules), but the
///    `::jsonb` cast is not valid SQLite, so JSON columns default to the plain
///    string `'{}'` here (same semantics: empty JSON = feature disabled).
///  - `BOOLEAN` is stored as INTEGER 0/1 (SQLite has no native bool).
extern const char* const kCatalogSchemaSql;

/// The catalog ISchemaProvider: owns the catalog.db framework schema (6 + 6 tables
/// + the 9 reserved `units` fields). Registered first with the SchemaMigrator
/// (ARCH §1.3.bis.3 topological order). Other Features register their own
/// providers for extension tables/columns.
class CatalogSchemaProvider : public ISchemaProvider {
public:
    std::string FeatureName() const override { return "catalog"; }
    int CurrentVersion() const override { return kCatalogSchemaVersion; }
    Status Migrate(sqlite3* db, int from_ver, int to_ver) override;
};

}  // namespace cortrix::catalog
