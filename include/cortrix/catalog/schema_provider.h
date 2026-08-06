#pragma once
#include <string>
#include <vector>

#include "cortrix/common/status.h"

// Forward declaration of the SQLite handle; the full <sqlite3.h> is pulled in by
// the .cpp and by providers that emit DDL. Keeps this widely-included shared
// header dependency-light.
typedef struct sqlite3 sqlite3;

namespace cortrix::catalog {

/// A Feature's schema contribution (scaffolding, interface SoT =
/// Each subsystem (catalog / parser / block header / write coordinator / enricher /
/// watcher / HyPE / doc summary / memory) implements one and registers it with the SchemaMigrator.
class ISchemaProvider {
public:
    virtual ~ISchemaProvider() = default;

    /// Stable Feature identifier (key in the schema_version table).
    virtual std::string FeatureName() const = 0;

    /// Target schema version this build expects.
    virtual int CurrentVersion() const = 0;

    /// Apply the migration steps to move `db` from `from_ver` to `to_ver`.
    /// Runs inside the migrator's transaction; return an error Status to abort
    /// and roll back the whole batch. (Note: void-fallible → Status, not
    /// Result<void>, per the coding conventions)
    virtual Status Migrate(sqlite3* db, int from_ver, int to_ver) = 0;
};

/// Central schema-migration orchestrator (scaffolding). Registers
/// providers, tracks per-Feature versions in a `schema_version` table, and runs
/// outstanding migrations in registration (dependency-topological) order inside
/// a single transaction — all-or-nothing.
class SchemaMigrator {
public:
    SchemaMigrator() = default;

    /// Register a provider. Order is the migration order; the catalog registers in the
    /// topological order (catalog first, then the per-Unit schema owners).
    void Register(ISchemaProvider* provider);

    /// Migrate the catalog database (catalog.db). Atomic across all providers.
    Status MigrateCatalog(sqlite3* db);

    /// Migrate a per-Unit database. Same engine, applied to the Unit's db.
    Status MigrateUnit(sqlite3* db, const std::string& unit_id);

    /// Current persisted version of `feature` in `db` (0 if never migrated).
    int CurrentVersion(sqlite3* db, const std::string& feature) const;

private:
    Status RunProviders(sqlite3* db);

    std::vector<ISchemaProvider*> providers_;
};

}  // namespace cortrix::catalog
