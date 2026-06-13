#include "cortrix/spc_enricher/f03_schema_provider.h"

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
            std::string("CX_ERR_SCHEMA_MIGRATION_FAILED: F03 ") + what + ": " + detail);
    }
    return Status::Ok();
}

// Add a single blocks column iff absent (SQLite ADD COLUMN is not "if not exists").
Status AddBlocksColumnIfAbsent(sqlite3* db, const char* column, const char* ddl,
                               const char* what) {
    if (ColumnExists(db, "blocks", column)) return Status::Ok();
    return Exec(db, ddl, what);
}

}  // namespace

Status F03SchemaProvider::Migrate(sqlite3* db, int from_ver, int to_ver) {
    const bool init = (from_ver == 0 && to_ver == 1);
    if (!init && from_ver != to_ver) {
        return Status::InvalidArgument(
            "CX_ERR_SCHEMA_VERSION_MISMATCH: F03 unsupported migration " +
            std::to_string(from_ver) + " -> " + std::to_string(to_ver));
    }
    if (!db) return Status::InvalidArgument("F03 migrate: null db");

    // blocks is the F09-owned per-Unit table, created before F03's provider runs.
    // If absent (isolated unit test not building the per-Unit framework), no-op so
    // the migrator batch isn't blocked.
    if (!TableExists(db, "blocks")) return Status::Ok();

    // --- per-Unit.blocks +3 columns (topic 2.6) — idempotent ---
    if (Status s = AddBlocksColumnIfAbsent(
            db, "enriched_score",
            "ALTER TABLE blocks ADD COLUMN enriched_score REAL DEFAULT NULL",
            "add enriched_score"); !s.ok()) {
        return s;
    }
    if (Status s = AddBlocksColumnIfAbsent(
            db, "enriched_at",
            "ALTER TABLE blocks ADD COLUMN enriched_at INTEGER DEFAULT NULL",
            "add enriched_at"); !s.ok()) {
        return s;
    }
    if (Status s = AddBlocksColumnIfAbsent(
            db, "enricher_metadata",
            "ALTER TABLE blocks ADD COLUMN enricher_metadata TEXT DEFAULT NULL",
            "add enricher_metadata"); !s.ok()) {
        return s;
    }
    // Partial index on enriched_score (only enriched rows) — CREATE IF NOT EXISTS.
    if (Status s = Exec(db,
            "CREATE INDEX IF NOT EXISTS idx_blocks_enriched_score "
            "ON blocks(enriched_score) WHERE enriched_score IS NOT NULL",
            "index enriched_score"); !s.ok()) {
        return s;
    }

    // --- entities table + indexes + FTS5 (topic 2.6, F03 self-maintained) — idempotent ---
    if (Status s = Exec(db,
            "CREATE TABLE IF NOT EXISTS entities ("
            "  entity_id INTEGER PRIMARY KEY,"
            "  block_id INTEGER NOT NULL REFERENCES blocks(block_id),"
            "  text TEXT NOT NULL,"
            "  type TEXT NOT NULL,"
            "  start_offset INTEGER,"
            "  end_offset INTEGER)",
            "create entities"); !s.ok()) {
        return s;
    }
    if (Status s = Exec(db,
            "CREATE INDEX IF NOT EXISTS idx_entities_block ON entities(block_id)",
            "index entities.block_id"); !s.ok()) {
        return s;
    }
    if (Status s = Exec(db,
            "CREATE INDEX IF NOT EXISTS idx_entities_type ON entities(type)",
            "index entities.type"); !s.ok()) {
        return s;
    }
    // FTS5 external-content index over (text, type) keyed to entities.entity_id.
    if (Status s = Exec(db,
            "CREATE VIRTUAL TABLE IF NOT EXISTS entities_fts USING fts5("
            "  text, type, content='entities', content_rowid='entity_id')",
            "create entities_fts"); !s.ok()) {
        return s;
    }
    return Status::Ok();
}

}  // namespace cortrix::spc
