#include "cortrix/scoring/scoring_schema_provider.h"

#include <string>

#include <sqlite3.h>

namespace cortrix::scoring {

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
            std::string("CX_ERR_SCHEMA_MIGRATION_FAILED: scoring ") + what + ": " + detail);
    }
    return Status::Ok();
}

}  // namespace

Status ScoringSchemaProvider::Migrate(sqlite3* db, int from_ver, int to_ver) {
    const bool init = (from_ver == 0 && to_ver == 1);
    if (!init && from_ver != to_ver) {
        return Status::InvalidArgument(
            "CX_ERR_SCHEMA_VERSION_MISMATCH: scoring unsupported migration " +
            std::to_string(from_ver) + " -> " + std::to_string(to_ver));
    }
    if (!db) return Status::InvalidArgument("scoring migrate: null db");

    // blocks is the block-header-owned per-Unit table, created before this provider runs. If
    // absent (isolated unit test not building the per-Unit framework), no-op so the
    // migrator batch isn't blocked (same guard as the enricher's provider).
    if (!TableExists(db, "blocks")) return Status::Ok();

    // semantic_score is a per-Unit blocks column (write-time score, immutable,
    // 5-level discrete float). SQLite ADD COLUMN is not "if not exists" → guard on ColumnExists for
    // idempotency (mirrors enriched_score).
    if (ColumnExists(db, "blocks", "semantic_score")) return Status::Ok();
    if (Status s = Exec(db,
            "ALTER TABLE blocks ADD COLUMN semantic_score REAL DEFAULT NULL",
            "add semantic_score"); !s.ok()) {
        return s;
    }
    // Partial index on scored rows (query-time JOIN reads semantic_score; only scored
    // rows indexed). CREATE IF NOT EXISTS.
    return Exec(db,
        "CREATE INDEX IF NOT EXISTS idx_blocks_semantic_score "
        "ON blocks(semantic_score) WHERE semantic_score IS NOT NULL",
        "index semantic_score");
}

}  // namespace cortrix::scoring
