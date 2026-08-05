#include "cortrix/spc/parser_schema_provider.h"

#include <string>

#include <sqlite3.h>

namespace cortrix::spc {

namespace {

// Whether `table` exists in the connected database.
bool TableExists(sqlite3* db, const char* table) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?1";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, table, -1, SQLITE_STATIC);
    const bool found = (sqlite3_step(stmt) == SQLITE_ROW);
    sqlite3_finalize(stmt);
    return found;
}

// Whether `table` already has a column named `column` (via pragma table_info).
bool ColumnExists(sqlite3* db, const char* table, const char* column) {
    // pragma_table_info is a table-valued function (SQLite ≥ 3.16) — usable in a
    // normal SELECT, which lets us bind/scan rather than parse PRAGMA output.
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

Status ParserSchemaProvider::Migrate(sqlite3* db, int from_ver, int to_ver) {
    // Only the Phase-1 init step (0 → 1) and the already-current (n → n) call are
    // supported; any other step is a version mismatch until a future bump.
    const bool init = (from_ver == 0 && to_ver == 1);
    if (!init && from_ver != to_ver) {
        return Status::InvalidArgument(
            "CX_ERR_SCHEMA_VERSION_MISMATCH: parser unsupported migration " +
            std::to_string(from_ver) + " -> " + std::to_string(to_ver));
    }
    if (!db) return Status::InvalidArgument("parser migrate: null db");

    // The catalog's `namespaces` table is created by the catalog schema before this
    // provider runs (registration order: catalog → parser). If it isn't present yet
    // (e.g. an isolated unit test that didn't build the catalog), there is
    // nothing to alter — succeed as a no-op so the migrator batch isn't blocked.
    if (!TableExists(db, "namespaces")) return Status::Ok();

    // Idempotent: add the column only if it isn't already there. SQLite's bare
    // ADD COLUMN is not "if not exists", so we gate on table_info.
    if (ColumnExists(db, "namespaces", "parser_config")) return Status::Ok();

    char* err = nullptr;
    const char* ddl =
        "ALTER TABLE namespaces ADD COLUMN parser_config TEXT DEFAULT '{}'";
    if (sqlite3_exec(db, ddl, nullptr, nullptr, &err) != SQLITE_OK) {
        std::string detail = err ? err : "unknown";
        sqlite3_free(err);
        return Status::Internal(
            "CX_ERR_SCHEMA_MIGRATION_FAILED: parser add parser_config: " + detail);
    }
    return Status::Ok();
}

}  // namespace cortrix::spc
