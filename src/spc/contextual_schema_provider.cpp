#include "cortrix/spc_enricher/contextual_schema_provider.h"

#include <string>

#include <sqlite3.h>

namespace cortrix::spc {

namespace {

bool TableExists(sqlite3* db, const char* table) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?1";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, table, -1, SQLITE_STATIC);
    const bool found = (sqlite3_step(stmt) == SQLITE_ROW);
    sqlite3_finalize(stmt);
    return found;
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

Status Exec(sqlite3* db, const char* ddl, const char* what) {
    char* err = nullptr;
    if (sqlite3_exec(db, ddl, nullptr, nullptr, &err) != SQLITE_OK) {
        std::string detail = err ? err : "unknown";
        sqlite3_free(err);
        return Status::Internal(
            std::string("CX_ERR_SCHEMA_MIGRATION_FAILED: contextual enricher ") + what + ": " + detail);
    }
    return Status::Ok();
}

Status AddBlocksColumnIfAbsent(sqlite3* db, const char* column, const char* ddl,
                               const char* what) {
    if (ColumnExists(db, "blocks", column)) return Status::Ok();
    return Exec(db, ddl, what);
}

}  // namespace

Status ContextualSchemaProvider::Migrate(sqlite3* db, int from_ver, int to_ver) {
    // Forward-only, idempotent (mirrors the enricher's multi-version pattern): a single
    // pass covers 0→1, 0→2 and the 1→2 label-table add; reject backward /
    // beyond-current steps as a mismatch.
    if (from_ver < 0 || to_ver > CurrentVersion() || to_ver < from_ver) {
        return Status::InvalidArgument(
            "CX_ERR_SCHEMA_VERSION_MISMATCH: contextual unsupported migration " +
            std::to_string(from_ver) + " -> " + std::to_string(to_ver));
    }
    if (!db) return Status::InvalidArgument("contextual migrate: null db");

    // --- V2: contextual_vec_labels (addendum §3.8 dual-vector ANN label map) ---
    // Independent of blocks (no FK) so it is created even in isolated unit tests;
    // rows are written by the ingest write phase / backfill worker alongside the
    // contextual vector point.
    if (Status s = Exec(db,
            "CREATE TABLE IF NOT EXISTS contextual_vec_labels ("
            "  label    INTEGER PRIMARY KEY,"
            "  block_id INTEGER NOT NULL,"
            "  child_id TEXT NOT NULL"
            ");"
            "CREATE INDEX IF NOT EXISTS idx_ctx_vec_labels_block"
            "  ON contextual_vec_labels(block_id);",
            "create contextual_vec_labels"); !s.ok()) {
        return s;
    }

    // blocks is the block-header-owned per-Unit table, created before this provider runs.
    // If absent (isolated unit test), no-op so the migrator batch isn't blocked.
    if (!TableExists(db, "blocks")) return Status::Ok();

    // --- blocks +4 contextual-retrieval columns (A unified-blocks § 4.1) — idempotent ---
    if (Status s = AddBlocksColumnIfAbsent(
            db, "embedding",
            "ALTER TABLE blocks ADD COLUMN embedding BLOB DEFAULT NULL",
            "add embedding"); !s.ok()) {
        return s;
    }
    if (Status s = AddBlocksColumnIfAbsent(
            db, "contextualized_text",
            "ALTER TABLE blocks ADD COLUMN contextualized_text TEXT DEFAULT NULL",
            "add contextualized_text"); !s.ok()) {
        return s;
    }
    if (Status s = AddBlocksColumnIfAbsent(
            db, "contextualized_embedding",
            "ALTER TABLE blocks ADD COLUMN contextualized_embedding BLOB DEFAULT NULL",
            "add contextualized_embedding"); !s.ok()) {
        return s;
    }
    if (Status s = AddBlocksColumnIfAbsent(
            db, "contextualized_status",
            "ALTER TABLE blocks ADD COLUMN contextualized_status SMALLINT DEFAULT 0",
            "add contextualized_status"); !s.ok()) {
        return s;
    }
    return Status::Ok();
}

}  // namespace cortrix::spc
