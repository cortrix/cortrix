#pragma once
#include <string>
#include <vector>

#include "cortrix/common/status.h"

typedef struct sqlite3 sqlite3;

namespace cortrix::catalog { class ISchemaProvider; }

namespace cortrix::auth {

/// Owns the platform.db SQLite handle (the Auth-layer SoT, P08 §3.1). Opens the
/// database with the §3.8 pragmas (WAL + foreign_keys + synchronous=NORMAL +
/// busy_timeout) and runs the Auth schema migration through the shared
/// SchemaMigrator (the FROZEN L0 scaffold, catalog/schema_provider.h): the
/// P08AuthSchemaProvider's 7 tables first, then any extra providers.
///
/// 🔒 platform.db is a SEPARATE database file from F12's catalog.db (see
/// auth_schema.h). This class deliberately does NOT register F12SchemaProvider —
/// the catalog tables do not belong in platform.db. Joining the two (one users
/// SoT) is cross-Feature wiring = D3.5, not this Story.
///
/// Mirrors catalog::CatalogDb verbatim in shape so the two composition roots read
/// the same. Not thread-safe to construct/Open concurrently; callers serialize
/// startup.
class PlatformDb {
public:
    PlatformDb() = default;
    ~PlatformDb();

    PlatformDb(const PlatformDb&) = delete;
    PlatformDb& operator=(const PlatformDb&) = delete;

    /// Open `db_path` (created if absent; use ":memory:" for tests), apply the
    /// §3.8 pragmas, then run the P08 Auth schema migration only. Idempotent: a
    /// second Open on an already-migrated file is a no-op (version-gated by the
    /// SchemaMigrator's schema_version table).
    Status Open(const std::string& db_path);

    /// Same as Open(db_path) but also registers `extra_providers` (in order,
    /// after P08) before migrating, so the whole platform.db schema is applied in
    /// one atomic batch. Pointers are borrowed for the duration of the call; the
    /// caller owns the provider objects. nullptr entries are ignored.
    Status Open(const std::string& db_path,
                const std::vector<catalog::ISchemaProvider*>& extra_providers);

    /// Close the handle (also called by the destructor). Safe to call twice.
    void Close();

    /// Raw handle for the Auth services (AuthService / JwtSecretService /
    /// AuthConfigService / ApiKeyService) built in later Stories. nullptr until a
    /// successful Open(). Ownership stays with this object.
    sqlite3* db() const { return db_; }

private:
    Status OpenAndConfigure(const std::string& db_path);

    sqlite3* db_ = nullptr;
};

}  // namespace cortrix::auth
