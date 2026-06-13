#include "cortrix/store/f09_schema_provider.h"

#include <sqlite3.h>

#include <string>

#include "cortrix/store/per_unit_schema_ddl.h"

namespace cortrix::store {

Status F09SchemaProvider::Migrate(sqlite3* db, int from_ver, int to_ver) {
    // [D3.5-B] F09 owns the per-Unit framework schema (documents / blocks /
    // blocks_fts + indices + triggers). 0 → 1 creates it from the single DDL SoT
    // (idempotent CREATE IF NOT EXISTS). Runs inside the SchemaMigrator txn on the
    // production MigrateUnit path; the same kPerUnitFrameworkDdl is reused by
    // CortrixStoreSqlite::Open for the standalone owns-db path.
    if (from_ver == to_ver) {
        return Status::Ok();  // already current
    }
    if (from_ver == 0 && to_ver == 1) {
        char* err = nullptr;
        if (sqlite3_exec(db, kPerUnitFrameworkDdl, nullptr, nullptr, &err) != SQLITE_OK) {
            std::string msg = err ? err : "unknown";
            sqlite3_free(err);
            return Status::Internal(
                "CX_ERR_SCHEMA_MIGRATION_FAILED: F09 per-unit framework DDL: " + msg);
        }
        return Status::Ok();
    }
    // Phase 2 placeholder: from_ver==1 && to_ver==2 → header V1→V2 upgrade
    // (TD-BLOCK-V2-UPGRADE). Until then, an unexpected version step is an error.
    return Status::InvalidArgument(
        "CX_ERR_SCHEMA_VERSION_MISMATCH: F09 unsupported migration " +
        std::to_string(from_ver) + " -> " + std::to_string(to_ver));
}

}  // namespace cortrix::store
