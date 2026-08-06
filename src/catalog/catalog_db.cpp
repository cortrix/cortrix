#include "cortrix/catalog/catalog_db.h"

#include <sqlite3.h>

#include <string>
#include <vector>

#include "cortrix/catalog/catalog_schema.h"
#include "cortrix/catalog/schema_provider.h"

namespace cortrix::catalog {

namespace {

Status ExecPragma(sqlite3* db, const char* sql) {
    char* err = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        std::string msg = err ? err : "sqlite error";
        sqlite3_free(err);
        return Status::Internal(std::string("catalog pragma failed: ") + msg);
    }
    return Status::Ok();
}

}  // namespace

CatalogDb::~CatalogDb() { Close(); }

Status CatalogDb::OpenAndConfigure(const std::string& db_path) {
    if (db_ != nullptr) {
        return Status::InvalidArgument("catalog.db already open");
    }

    if (sqlite3_open(db_path.c_str(), &db_) != SQLITE_OK) {
        // sqlite3_open can allocate a handle even on failure; capture the message
        // and clean up so the object stays in a closed state.
        std::string msg = db_ ? sqlite3_errmsg(db_) : "sqlite3_open failed";
        sqlite3_close(db_);
        db_ = nullptr;
        return Status::Internal(std::string("catalog.db open failed: ") + msg);
    }

    // WAL for concurrent readers + crash safety. foreign_keys ON so
    // the FK constraints in the schema are actually enforced (off by default in
    // SQLite). A ':memory:' db reports WAL as 'memory'; that is expected.
    Status st = ExecPragma(db_, "PRAGMA journal_mode=WAL;");
    if (st.ok()) st = ExecPragma(db_, "PRAGMA foreign_keys=ON;");
    if (!st.ok()) {
        Close();
        return st;
    }
    return Status::Ok();
}

Status CatalogDb::Open(const std::string& db_path) {
    return Open(db_path, {});
}

Status CatalogDb::Open(const std::string& db_path,
                       const std::vector<ISchemaProvider*>& extra_providers) {
    Status st = OpenAndConfigure(db_path);
    if (!st.ok()) return st;

    // Run the whole catalog schema through the shared migrator in one atomic
    // batch. The catalog is registered FIRST so its base tables (units / tenants /
    // namespaces) — the FK targets the rest reference — exist before any
    // downstream Feature provider extends them. The caller supplies the
    // downstream providers already in ARCH topological order;
    // SchemaMigrator preserves registration order and skips nullptr entries.
    CatalogSchemaProvider catalog_provider;
    SchemaMigrator migrator;
    migrator.Register(&catalog_provider);
    for (ISchemaProvider* p : extra_providers) {
        migrator.Register(p);
    }
    st = migrator.MigrateCatalog(db_);
    if (!st.ok()) {
        Close();
        return st;
    }
    return Status::Ok();
}

void CatalogDb::Close() {
    if (db_ != nullptr) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

}  // namespace cortrix::catalog
