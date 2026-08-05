#include "cortrix/retrieval/f40_schema_provider.h"

#include <sqlite3.h>

#include <string>

namespace cortrix::retrieval {

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

}  // namespace

Status F40SchemaProvider::Migrate(sqlite3* db, int from_ver, int to_ver) {
    // Phase 1: 0 → 1 creates the sparse_inverted_index table AND adds
    // the blocks.sparse_vec BLOB column (A unified-blocks: sparse_vec lives on child
    // rows of `blocks`, not the legacy children table). Accept an already-current
    // (1 → 1) call defensively (all DDL is idempotent). Runs inside the
    // SchemaMigrator's transaction.
    if (from_ver == to_ver) {
        return Status::Ok();  // already current
    }
    if (from_ver == 0 && to_ver == 1) {
        if (db == nullptr) {
            return Status::InvalidArgument(
                "CX_ERR_SCHEMA_VERSION_MISMATCH: F40 Migrate got null db");
        }
        // sparse_inverted_index (per-NS SPLADE postings; child_id is a logical FK to
        // blocks.child_id under A — partial unique index can't bind a hard FK).
        char* err = nullptr;
        int rc = sqlite3_exec(db, kSparseInvertedIndexDdl, nullptr, nullptr, &err);
        if (rc != SQLITE_OK) {
            std::string msg = err ? err : "DDL failed";
            sqlite3_free(err);
            return Status::Internal(
                "CX_ERR_F40_INVERTED_INDEX_WRITE_FAILED: create "
                "sparse_inverted_index: " + msg);
        }
        // [A unified-blocks] blocks.sparse_vec BLOB (F40-2 A) — only child rows write
        // it; other block_types leave it NULL. If `blocks` is absent (isolated unit
        // test without the per-Unit framework) skip the ALTER (the inverted index is
        // already created); the column is added once F09 builds blocks.
        if (TableExists(db, "blocks") && !ColumnExists(db, "blocks", "sparse_vec")) {
            rc = sqlite3_exec(db, "ALTER TABLE blocks ADD COLUMN sparse_vec BLOB",
                              nullptr, nullptr, &err);
            if (rc != SQLITE_OK) {
                std::string msg = err ? err : "DDL failed";
                sqlite3_free(err);
                return Status::Internal(
                    "CX_ERR_F40_INVERTED_INDEX_WRITE_FAILED: add blocks.sparse_vec: " + msg);
            }
        }
        return Status::Ok();
    }
    // Phase 2 (IVF-sparse / sharding, §14) is the only future step; until it is
    // defined an unexpected version jump is an error.
    return Status::InvalidArgument(
        "CX_ERR_SCHEMA_VERSION_MISMATCH: F40 unsupported migration " +
        std::to_string(from_ver) + " -> " + std::to_string(to_ver));
}

}  // namespace cortrix::retrieval
