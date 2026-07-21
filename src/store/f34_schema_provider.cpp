#include "cortrix/store/f34_schema_provider.h"

#include <string>

#include <sqlite3.h>

#include "cortrix/store/parent_chunk_schema.h"

namespace cortrix::store {

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
            std::string("CX_ERR_SCHEMA_MIGRATION_FAILED: F34 ") + what + ": " + detail);
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

Status F34SchemaProvider::Migrate(sqlite3* db, int from_ver, int to_ver) {
    const bool init = (from_ver == 0 && to_ver == 1);
    if (!init && from_ver != to_ver) {
        return Status::InvalidArgument(
            "CX_ERR_SCHEMA_VERSION_MISMATCH: F34 unsupported migration " +
            std::to_string(from_ver) + " -> " + std::to_string(to_ver));
    }
    if (!db) return Status::InvalidArgument("F34 migrate: null db");

    // --- parents table (kept under A; parent_text reverse-lookup, § D6) ---
    // Created unconditionally (independent of blocks). Idempotent (IF NOT EXISTS).
    if (Status s = Exec(db, kParentsSchemaSql, "create parents"); !s.ok()) {
        return s;
    }

    // blocks is the F09-owned per-Unit table, created before F34's provider runs.
    // If absent (isolated unit test not building the per-Unit framework), no-op the
    // blocks ALTERs so the migrator batch isn't blocked (parents is already created).
    if (!TableExists(db, "blocks")) return Status::Ok();

    // --- blocks +4 F34 child columns (A unified-blocks § 3.1) — idempotent ---
    // child_id is the business ULID AND the child-identity key (block_id =
    // HashChildIdToBlockId(child_id)); a child row is one with child_id IS NOT NULL,
    // its block_type being the source modality (no kBlockChild enum, candidate B).
    if (Status s = AddBlocksColumnIfAbsent(
            db, "child_id",
            "ALTER TABLE blocks ADD COLUMN child_id TEXT DEFAULT NULL",
            "add child_id"); !s.ok()) {
        return s;
    }
    if (Status s = AddBlocksColumnIfAbsent(
            db, "parent_id",
            "ALTER TABLE blocks ADD COLUMN parent_id TEXT DEFAULT NULL",
            "add parent_id"); !s.ok()) {
        return s;
    }
    if (Status s = AddBlocksColumnIfAbsent(
            db, "token_count",
            "ALTER TABLE blocks ADD COLUMN token_count INTEGER DEFAULT NULL",
            "add token_count"); !s.ok()) {
        return s;
    }
    if (Status s = AddBlocksColumnIfAbsent(
            db, "parent_offset",
            "ALTER TABLE blocks ADD COLUMN parent_offset INTEGER DEFAULT NULL",
            "add parent_offset"); !s.ok()) {
        return s;
    }

    // --- F34 indexes (partial; CREATE IF NOT EXISTS) ---
    // child identity: UNIQUE over child_id (the partial unique that carries "is a
    // child" — F40 sparse inverted index references child_id logically against this).
    if (Status s = Exec(db,
            "CREATE UNIQUE INDEX IF NOT EXISTS idx_blocks_child_id "
            "ON blocks(child_id) WHERE child_id IS NOT NULL",
            "index child_id"); !s.ok()) {
        return s;
    }
    // parent reverse-lookup (children of a parent).
    if (Status s = Exec(db,
            "CREATE INDEX IF NOT EXISTS idx_blocks_parent "
            "ON blocks(parent_id) WHERE parent_id IS NOT NULL",
            "index parent_id"); !s.ok()) {
        return s;
    }
    // F08 "1 doc = 1 META" uniqueness (built by F34SP; semantics
    // belong to F08). block_type = 8 = kBlockMeta (F09 CortrixBlockType enum).
    if (Status s = Exec(db,
            "CREATE UNIQUE INDEX IF NOT EXISTS idx_blocks_meta_doc "
            "ON blocks(doc_id) WHERE block_type = 8",
            "index meta_doc"); !s.ok()) {
        return s;
    }
    return Status::Ok();
}

}  // namespace cortrix::store
