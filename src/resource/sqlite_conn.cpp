#include "cortrix/resource/sqlite_conn.h"

#include <sqlite3.h>

#include <string>
#include <utility>

namespace cortrix::resource {

SqliteConn::~SqliteConn() { Close(); }

SqliteConn::SqliteConn(SqliteConn&& other) noexcept
    : db_(other.db_), path_(std::move(other.path_)) {
    other.db_ = nullptr;
}

SqliteConn& SqliteConn::operator=(SqliteConn&& other) noexcept {
    if (this != &other) {
        Close();
        db_ = other.db_;
        path_ = std::move(other.path_);
        other.db_ = nullptr;
    }
    return *this;
}

void SqliteConn::Close() {
    if (db_ != nullptr) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

Status SqliteConn::Open(const std::string& db_path) {
    Close();
    int rc = sqlite3_open(db_path.c_str(), &db_);
    if (rc != SQLITE_OK) {
        // sqlite3_open allocates the handle even on failure; capture the message
        // then close so we never leak it. Status carries the CX_ERR_NS_LOAD_FAILED
        // token so the pool maps it to PoolErrorCode::kNsLoadFailed.
        std::string detail = db_ ? sqlite3_errmsg(db_) : "sqlite3_open failed";
        Close();
        return Status(StatusCode::kUnavailable,
                      "CX_ERR_NS_LOAD_FAILED: open store.db '" + db_path +
                          "': " + detail);
    }
    path_ = db_path;
    return Status::Ok();
}

Status SqliteConn::ApplyPragmas(const SqlitePragmas& pragmas) {
    if (db_ == nullptr) return Status::Ok();  // nothing to configure yet

    // PRAGMA failures degrade performance but do not corrupt data, so a
    // failed PRAGMA is logged-and-tolerated rather than fatal. We still surface
    // the first failure as a non-Ok Status so callers can log it; the connection
    // stays usable.
    const std::string stmts[] = {
        // busy_timeout FIRST: it only guards statements that run AFTER it (a
        // fresh db's WAL conversion takes an exclusive lock; without the timeout
        // already set, a concurrent opener fails immediately with
        // "database is locked" instead of queueing).
        "PRAGMA busy_timeout=" + std::to_string(pragmas.busy_timeout_ms),
        "PRAGMA journal_mode=" + pragmas.journal_mode,
        "PRAGMA synchronous=" + pragmas.synchronous,
        "PRAGMA cache_size=" + std::to_string(pragmas.cache_size_kb),
        "PRAGMA mmap_size=" + std::to_string(pragmas.mmap_size_bytes),
        "PRAGMA temp_store=" + pragmas.temp_store,
    };
    Status first_error = Status::Ok();
    for (const std::string& stmt : stmts) {
        char* errmsg = nullptr;
        int rc = sqlite3_exec(db_, stmt.c_str(), nullptr, nullptr, &errmsg);
        if (rc != SQLITE_OK && first_error.ok()) {
            first_error = Status(StatusCode::kInternal,
                                 "PRAGMA failed: " + stmt + ": " +
                                     (errmsg ? errmsg : "unknown"));
        }
        if (errmsg) sqlite3_free(errmsg);
    }
    return first_error;
}

Result<std::string> SqliteConn::ReadPragma(const std::string& pragma_name) const {
    if (db_ == nullptr) {
        return Status(StatusCode::kInternal, "ReadPragma on closed connection");
    }
    std::string sql = "PRAGMA " + pragma_name;
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return Status(StatusCode::kInternal,
                      "ReadPragma prepare failed: " + sql + ": " +
                          sqlite3_errmsg(db_));
    }
    std::string value;
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        const unsigned char* text = sqlite3_column_text(stmt, 0);
        if (text) value = reinterpret_cast<const char*>(text);
    }
    sqlite3_finalize(stmt);
    return Result<std::string>(std::move(value));
}

}  // namespace cortrix::resource
