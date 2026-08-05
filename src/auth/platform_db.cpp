#include "cortrix/auth/platform_db.h"

#include <sqlite3.h>

#include <string>
#include <vector>

#include "cortrix/auth/auth_schema.h"
#include "cortrix/catalog/schema_provider.h"

namespace cortrix::auth {

namespace {

Status ExecPragma(sqlite3* db, const char* sql) {
    char* err = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        std::string msg = err ? err : "sqlite error";
        sqlite3_free(err);
        return Status::Internal(std::string("platform.db pragma failed: ") + msg);
    }
    return Status::Ok();
}

}  // namespace

PlatformDb::~PlatformDb() { Close(); }

Status PlatformDb::OpenAndConfigure(const std::string& db_path) {
    if (db_ != nullptr) {
        return Status::InvalidArgument("platform.db already open");
    }

    if (sqlite3_open(db_path.c_str(), &db_) != SQLITE_OK) {
        std::string msg = db_ ? sqlite3_errmsg(db_) : "sqlite3_open failed";
        sqlite3_close(db_);
        db_ = nullptr;
        return Status::Internal(std::string("platform.db open failed: ") + msg);
    }

    // Auth-db pragmas (same family as the NS store.db). A ':memory:' db reports
    // WAL as 'memory'; that is expected. foreign_keys ON so the FK constraints in
    // the Auth schema (refresh_tokens / api_keys → users) are enforced.
    Status st = ExecPragma(db_, "PRAGMA journal_mode=WAL;");
    if (st.ok()) st = ExecPragma(db_, "PRAGMA synchronous=NORMAL;");
    if (st.ok()) st = ExecPragma(db_, "PRAGMA foreign_keys=ON;");
    if (st.ok()) st = ExecPragma(db_, "PRAGMA busy_timeout=5000;");
    if (!st.ok()) {
        Close();
        return st;
    }
    return Status::Ok();
}

Status PlatformDb::Open(const std::string& db_path) {
    return Open(db_path, {});
}

Status PlatformDb::Open(const std::string& db_path,
                        const std::vector<catalog::ISchemaProvider*>& extra_providers) {
    Status st = OpenAndConfigure(db_path);
    if (!st.ok()) return st;

    // Run the platform.db schema through the shared (frozen L0) migrator in one
    // atomic batch. Auth is registered FIRST so its `users` table (the FK target
    // refresh_tokens / api_keys reference) exists before any extra provider.
    // NOTE: CatalogSchemaProvider is intentionally NOT registered here — catalog
    // tables live in catalog.db, a different file (D3.5 reconciles the two).
    AuthSchemaProvider p08_provider;
    catalog::SchemaMigrator migrator;
    migrator.Register(&p08_provider);
    for (catalog::ISchemaProvider* p : extra_providers) {
        migrator.Register(p);
    }
    // MigrateCatalog == "run all registered providers atomically"; the method
    // name is the migrator's generic batch entry (it is DB-agnostic — same engine
    // the per-Unit path uses), here applied to platform.db.
    st = migrator.MigrateCatalog(db_);
    if (!st.ok()) {
        Close();
        return st;
    }
    return Status::Ok();
}

void PlatformDb::Close() {
    if (db_ != nullptr) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

}  // namespace cortrix::auth
