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

bool ColumnExists(sqlite3* db, const char* table, const char* column) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT 1 FROM pragma_table_info(?1) WHERE name=?2";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, table, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, column, -1, SQLITE_STATIC);
    const bool found = (sqlite3_step(stmt) == SQLITE_ROW);
    sqlite3_finalize(stmt);
    return found;
}

}  // namespace

Status EnrichStateSchemaProvider::Migrate(sqlite3* db, int from_ver, int to_ver) {
    // Forward-only, idempotent: one pass covers 0→1, 0→2 and 1→2; backward or
    // beyond-current steps are a mismatch.
    if (from_ver < 0 || to_ver > CurrentVersion() || to_ver < from_ver) {
        return Status::InvalidArgument(
            "CX_ERR_SCHEMA_VERSION_MISMATCH: EnrichState unsupported migration " +
            std::to_string(from_ver) + " -> " + std::to_string(to_ver));
    }
    if (!db) return Status::InvalidArgument("EnrichState migrate: null db");

    // status: 'ok' | 'pending_retry' | 'failed_permanent'.
    // failed_members: csv of owed chain tokens still missing (e.g. "f03,f38").
    // next_retry_at: unix seconds; set only while status='pending_retry' (the
    // sweeper's due gate + lease field).
    // generation (V2): bumped by the operator reset and by the audit's settle and
    // re-owe; a delivery writes a row's outcome only while the row still carries
    // the generation the delivery read (enrich_state_store.h, "Generation").
    Status s = Exec(db,
                    "CREATE TABLE IF NOT EXISTS enrich_state ("
                    "  block_id       INTEGER PRIMARY KEY,"
                    "  doc_id         TEXT NOT NULL,"
                    "  child_id       TEXT NOT NULL,"
                    "  status         TEXT NOT NULL,"
                    "  failed_members TEXT,"
                    "  attempts       INTEGER NOT NULL DEFAULT 0,"
                    "  last_error     TEXT,"
                    "  next_retry_at  INTEGER,"
                    "  updated_at     INTEGER NOT NULL,"
                    "  generation     INTEGER NOT NULL DEFAULT 0"
                    ");"
                    "CREATE INDEX IF NOT EXISTS idx_enrich_state_due"
                    "  ON enrich_state(status, next_retry_at);"
                    "CREATE INDEX IF NOT EXISTS idx_enrich_state_doc"
                    "  ON enrich_state(doc_id);",
                    "create enrich_state");
    if (!s.ok()) return s;
    if (to_ver < 2) return Status::Ok();

    // --- V2: generation, for tables created at V1 (a fresh V2 table has it). ---
    if (!ColumnExists(db, "enrich_state", "generation")) {
        s = Exec(db, "ALTER TABLE enrich_state ADD COLUMN generation INTEGER NOT NULL DEFAULT 0",
                 "add enrich_state.generation");
        if (!s.ok()) return s;
    }
    return Status::Ok();
}

}  // namespace cortrix::spc
