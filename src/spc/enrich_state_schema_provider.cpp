#include "cortrix/spc_enricher/enrich_state_schema_provider.h"

#include <string>

#include <sqlite3.h>

namespace cortrix::spc {

namespace {

Status Exec(sqlite3* db, const char* ddl, const char* what) {
    char* err = nullptr;
    if (sqlite3_exec(db, ddl, nullptr, nullptr, &err) != SQLITE_OK) {
        std::string detail = err ? err : "unknown";
        sqlite3_free(err);
        return Status::Internal(
            std::string("CX_ERR_SCHEMA_MIGRATION_FAILED: EnrichState ") + what + ": " +
            detail);
    }
    return Status::Ok();
}

}  // namespace

Status EnrichStateSchemaProvider::Migrate(sqlite3* db, int from_ver, int to_ver) {
    const bool init = (from_ver == 0 && to_ver == 1);
    if (!init && from_ver != to_ver) {
        return Status::InvalidArgument(
            "CX_ERR_SCHEMA_VERSION_MISMATCH: EnrichState unsupported migration " +
            std::to_string(from_ver) + " -> " + std::to_string(to_ver));
    }
    if (!db) return Status::InvalidArgument("EnrichState migrate: null db");

    // status: 'ok' | 'pending_retry' | 'failed_permanent'.
    // failed_members: csv of owed chain tokens still missing (e.g. "f03,f38").
    // next_retry_at: unix seconds; set only while status='pending_retry' (the
    // sweeper's due gate + lease field).
    return Exec(db,
                "CREATE TABLE IF NOT EXISTS enrich_state ("
                "  block_id       INTEGER PRIMARY KEY,"
                "  doc_id         TEXT NOT NULL,"
                "  child_id       TEXT NOT NULL,"
                "  status         TEXT NOT NULL,"
                "  failed_members TEXT,"
                "  attempts       INTEGER NOT NULL DEFAULT 0,"
                "  last_error     TEXT,"
                "  next_retry_at  INTEGER,"
                "  updated_at     INTEGER NOT NULL"
                ");"
                "CREATE INDEX IF NOT EXISTS idx_enrich_state_due"
                "  ON enrich_state(status, next_retry_at);"
                "CREATE INDEX IF NOT EXISTS idx_enrich_state_doc"
                "  ON enrich_state(doc_id);",
                "create enrich_state");
}

}  // namespace cortrix::spc
