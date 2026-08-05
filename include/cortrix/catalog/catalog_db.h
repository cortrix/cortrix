#pragma once
#include <string>
#include <vector>

#include "cortrix/common/result.h"
#include "cortrix/common/status.h"

typedef struct sqlite3 sqlite3;

namespace cortrix::catalog {

class ISchemaProvider;  // cortrix/catalog/schema_provider.h

/// Owns the catalog.db SQLite handle (the routing-layer SoT, F12 §1). Opens the
/// database in WAL mode with foreign-key enforcement and runs the catalog schema
/// migration through the shared SchemaMigrator: F12 first (6 main + 6 association
/// tables + 9 reserved `units` fields), then any additional Feature providers.
///
/// Provider ordering (S1.2/S1.3): F12 is always registered first so the base
/// tables its FK targets reference (units / tenants / namespaces) exist before
/// downstream providers extend them. The
/// caller passes the downstream providers in the ARCH §1.3.bis.3 topological
/// order; the migrator runs them in that registration order, atomically.
///
/// Scope so far: open + framework-schema init + multi-provider registration.
/// The routing interfaces (INSRouter / IUnitRouter / IContentRefStore /
/// IBloomFilter, F12 §3) layer on top in Wave 2 — this class exposes the raw
/// handle for them via db().
///
/// Not thread-safe to construct/Open concurrently; callers serialize startup.
class CatalogDb {
public:
    CatalogDb() = default;
    ~CatalogDb();

    CatalogDb(const CatalogDb&) = delete;
    CatalogDb& operator=(const CatalogDb&) = delete;

    /// Open `db_path` (created if absent; use ":memory:" for tests), apply WAL +
    /// foreign_keys pragmas, then run the F12 schema migration only. Idempotent:
    /// a second Open on an already-migrated file is a no-op (version-gated).
    Status Open(const std::string& db_path);

    /// Same as Open(db_path) but also registers `extra_providers` (in order,
    /// after F12) before migrating, so the whole catalog schema — F12 base +
    /// every Feature extension — is applied in one atomic SchemaMigrator batch.
    /// Pointers are borrowed for the duration of the call (not retained); the
    /// caller owns the provider objects and keeps them alive across the call.
    /// nullptr entries are ignored (SchemaMigrator::Register skips them).
    Status Open(const std::string& db_path,
                const std::vector<ISchemaProvider*>& extra_providers);

    /// Close the handle (also called by the destructor). Safe to call twice.
    void Close();

    /// Raw handle for the routing interfaces built in later Stories. nullptr
    /// until a successful Open(). Ownership stays with this object.
    sqlite3* db() const { return db_; }

private:
    /// Open + pragmas only (no migration). Leaves db_ set on success.
    Status OpenAndConfigure(const std::string& db_path);

    sqlite3* db_ = nullptr;
};

}  // namespace cortrix::catalog
