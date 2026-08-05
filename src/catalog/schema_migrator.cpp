#include "cortrix/catalog/schema_provider.h"

#include <sqlite3.h>

#include <string>

namespace cortrix::catalog {

namespace {

Status Exec(sqlite3* db, const char* sql) {
    char* err = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        std::string msg = err ? err : "sqlite error";
        sqlite3_free(err);
        return Status::Internal(
            std::string("CX_ERR_SCHEMA_MIGRATION_FAILED: schema migration failed: ") + msg);
    }
    return Status::Ok();
}

Status WriteVersion(sqlite3* db, const std::string& feature, int version) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "INSERT INTO schema_version(feature, version) VALUES(?, ?) "
        "ON CONFLICT(feature) DO UPDATE SET version = excluded.version";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return Status::Internal("schema_version upsert prepare failed");
    }
    sqlite3_bind_text(stmt, 1, feature.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, version);
    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        return Status::Internal("schema_version upsert failed");
    }
    return Status::Ok();
}

}  // namespace

void SchemaMigrator::Register(ISchemaProvider* provider) {
    if (provider != nullptr) {
        providers_.push_back(provider);
    }
}

int SchemaMigrator::CurrentVersion(sqlite3* db, const std::string& feature) const {
    sqlite3_stmt* stmt = nullptr;
    int version = 0;
    if (sqlite3_prepare_v2(db, "SELECT version FROM schema_version WHERE feature = ?", -1,
                           &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, feature.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            version = sqlite3_column_int(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }
    return version;
}

Status SchemaMigrator::RunProviders(sqlite3* db) {
    // The version table lives outside the migration transaction so it survives a
    // rollback (the bookkeeping table must persist even when a batch aborts).
    Status st = Exec(db,
                     "CREATE TABLE IF NOT EXISTS schema_version ("
                     "feature TEXT PRIMARY KEY, version INTEGER NOT NULL)");
    if (!st.ok()) return st;

    st = Exec(db, "BEGIN");
    if (!st.ok()) return st;

    for (ISchemaProvider* provider : providers_) {
        const std::string feature = provider->FeatureName();
        const int from = CurrentVersion(db, feature);
        const int to = provider->CurrentVersion();
        if (from >= to) continue;  // already up to date

        st = provider->Migrate(db, from, to);
        if (!st.ok()) {
            Exec(db, "ROLLBACK");
            return st;
        }
        st = WriteVersion(db, feature, to);
        if (!st.ok()) {
            Exec(db, "ROLLBACK");
            return st;
        }
    }

    return Exec(db, "COMMIT");
}

Status SchemaMigrator::MigrateCatalog(sqlite3* db) {
    return RunProviders(db);
}

Status SchemaMigrator::MigrateUnit(sqlite3* db, const std::string& /*unit_id*/) {
    // Same engine applied to a per-Unit database. The catalog refines which
    // providers target catalog vs per-Unit schema when it registers them; the
    // unit_id is reserved for that routing + logging context.
    return RunProviders(db);
}

}  // namespace cortrix::catalog
