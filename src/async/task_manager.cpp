#include <cstdint>
#include "cortrix/async/task_manager.h"

#include <sqlite3.h>

#include <chrono>
#include <cstdio>
#include <ctime>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "cortrix/async/f42_error.h"
#include "cortrix/id/ulid.h"

namespace cortrix::async {

namespace {

int ExecSQL(sqlite3* db, const char* sql) {
    char* err = nullptr;
    int rc = sqlite3_exec(db, sql, nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        spdlog::error("tasks SQL exec failed: {} — {}", err ? err : "unknown", sql);
        sqlite3_free(err);
    }
    return rc;
}

/// Whether `table` already has a column named `column` (via pragma_table_info).
/// SQLite ADD COLUMN is not "if not exists"; gating on this avoids the noisy
/// "duplicate column name" exec error logged on every existing/fresh DB.
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

std::string SafeColText(sqlite3_stmt* stmt, int col) {
    const unsigned char* t = sqlite3_column_text(stmt, col);
    return t ? reinterpret_cast<const char*>(t) : "";
}

/// ISO8601 UTC millisecond timestamp ("2026-05-31T10:00:00.123Z"). Matches the
/// MemoryStore / catalog convention so created_at/updated_at sort lexically.
std::string NowIso8601() {
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  now.time_since_epoch()) % 1000;
    std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf{};
    gmtime_r(&tt, &tm_buf);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                  tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
                  tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec,
                  static_cast<int>(ms.count()));
    return std::string(buf);
}

/// Convert a Unix-epoch-seconds instant to the same ISO8601 string format, so
/// cleanup cutoffs compare directly against created_at/updated_at (string ≤).
std::string UnixToIso8601(int64_t unix_secs) {
    std::time_t tt = static_cast<std::time_t>(unix_secs);
    std::tm tm_buf{};
    gmtime_r(&tt, &tm_buf);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.000Z",
                  tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
                  tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);
    return std::string(buf);
}

/// failed_pages vector<int> → JSON array string for storage; tolerant inverse.
std::string FailedPagesToJson(const std::vector<int>& pages) {
    nlohmann::json arr = nlohmann::json::array();
    for (int p : pages) arr.push_back(p);
    return arr.dump();
}

std::vector<int> FailedPagesFromJson(const std::string& s) {
    std::vector<int> out;
    if (s.empty()) return out;
    auto parsed = nlohmann::json::parse(s, nullptr, /*allow_exceptions=*/false);
    if (parsed.is_array()) {
        for (const auto& e : parsed) {
            if (e.is_number_integer()) out.push_back(e.get<int>());
        }
    }
    return out;
}

/// Bind a std::string, mapping empty → SQL NULL for the nullable text columns
/// (doc_id / current_phase / trace_id / error_code / ...), so absent values are
/// NULL rather than "". Required columns are bound with BindText() (never empty).
void BindNullableText(sqlite3_stmt* stmt, int idx, const std::string& v) {
    if (v.empty()) {
        sqlite3_bind_null(stmt, idx);
    } else {
        sqlite3_bind_text(stmt, idx, v.c_str(), -1, SQLITE_TRANSIENT);
    }
}

void BindText(sqlite3_stmt* stmt, int idx, const std::string& v) {
    sqlite3_bind_text(stmt, idx, v.c_str(), -1, SQLITE_TRANSIENT);
}

/// Column order shared by every SELECT in this file, materialized into TaskInfo.
/// 0 task_id, 1 namespace_id, 2 filename, 3 filepath, 4 doc_id, 5 content_hash,
/// 6 status, 7 task_type, 8 cancel_requested, 9 total_pages, 10 processed_pages,
/// 11 failed_pages, 12 progress_pct, 13 eta_seconds, 14 current_phase,
/// 15 worker_id, 16 trace_id, 17 error_code, 18 error_msg, 19 structured_data,
/// 20 created_at, 21 updated_at, 22 started_at, 23 completed_at, 24 metadata_json.
constexpr const char* kSelectColumns =
    "task_id, namespace_id, filename, filepath, doc_id, content_hash, status, "
    "task_type, cancel_requested, total_pages, processed_pages, failed_pages, "
    "progress_pct, eta_seconds, current_phase, worker_id, trace_id, error_code, "
    "error_msg, structured_data, created_at, updated_at, started_at, completed_at, "
    "metadata_json";

TaskInfo ReadRow(sqlite3_stmt* stmt) {
    TaskInfo t;
    t.task_id          = SafeColText(stmt, 0);
    t.namespace_id     = SafeColText(stmt, 1);
    t.filename         = SafeColText(stmt, 2);
    t.filepath         = SafeColText(stmt, 3);
    t.doc_id           = SafeColText(stmt, 4);
    t.content_hash     = SafeColText(stmt, 5);
    t.status           = SafeColText(stmt, 6);
    t.task_type        = sqlite3_column_int(stmt, 7);
    t.cancel_requested = sqlite3_column_int(stmt, 8) != 0;
    t.total_pages      = sqlite3_column_int(stmt, 9);
    t.processed_pages  = sqlite3_column_int(stmt, 10);
    t.failed_pages     = FailedPagesFromJson(SafeColText(stmt, 11));
    t.progress_pct     = static_cast<float>(sqlite3_column_double(stmt, 12));
    t.eta_seconds      = sqlite3_column_int(stmt, 13);
    t.current_phase    = SafeColText(stmt, 14);
    // worker_id: NULL → -1 (unassigned).
    t.worker_id = sqlite3_column_type(stmt, 15) == SQLITE_NULL
                      ? -1
                      : sqlite3_column_int(stmt, 15);
    t.trace_id         = SafeColText(stmt, 16);
    t.error_code       = SafeColText(stmt, 17);
    t.error_msg        = SafeColText(stmt, 18);
    t.structured_data  = SafeColText(stmt, 19);
    t.created_at       = SafeColText(stmt, 20);
    t.updated_at       = SafeColText(stmt, 21);
    t.started_at       = SafeColText(stmt, 22);
    t.completed_at     = SafeColText(stmt, 23);
    t.metadata_json    = SafeColText(stmt, 24);
    return t;
}

}  // namespace

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

TaskManager::~TaskManager() {
    if (db_ && owns_db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

Status TaskManager::Init(const std::string& db_path) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (db_) return Status::Ok();  // idempotent

    int rc = sqlite3_open(db_path.c_str(), &db_);
    if (rc != SQLITE_OK) {
        std::string msg = sqlite3_errmsg(db_);
        sqlite3_close(db_);
        db_ = nullptr;
        return F42Status(F42ErrorCode::kStorageFailed, "open tasks db: " + msg);
    }
    owns_db_ = true;
    ExecSQL(db_, "PRAGMA journal_mode=WAL;");
    ExecSQL(db_, "PRAGMA foreign_keys=ON;");
    return CreateTasksTable();
}

Status TaskManager::CreateTasksTable() {
    // F42 §3.1 tasks table (SQLite dialect: BOOLEAN→INTEGER 0/1, JSON→TEXT).
    const char* sql = R"(
        CREATE TABLE IF NOT EXISTS tasks (
            task_id          TEXT PRIMARY KEY,
            namespace_id     TEXT NOT NULL,
            filename         TEXT NOT NULL,
            filepath         TEXT NOT NULL,
            doc_id           TEXT,
            content_hash     TEXT,
            status           TEXT NOT NULL DEFAULT 'queued',
            task_type        INTEGER NOT NULL DEFAULT 1,
            cancel_requested INTEGER NOT NULL DEFAULT 0,
            total_pages      INTEGER DEFAULT 0,
            processed_pages  INTEGER DEFAULT 0,
            failed_pages     TEXT DEFAULT '[]',
            progress_pct     REAL DEFAULT 0.0,
            eta_seconds      INTEGER DEFAULT -1,
            current_phase    TEXT,
            worker_id        INTEGER,
            trace_id         TEXT,
            error_code       TEXT,
            error_msg        TEXT,
            structured_data  TEXT,
            created_at       TEXT NOT NULL,
            updated_at       TEXT NOT NULL,
            started_at       TEXT,
            completed_at     TEXT,
            metadata_json    TEXT,
            CHECK (status IN ('queued','processing','cancelling','completed','failed','cancelled'))
        );
        CREATE INDEX IF NOT EXISTS idx_tasks_namespace  ON tasks(namespace_id);
        CREATE INDEX IF NOT EXISTS idx_tasks_status     ON tasks(status);
        CREATE INDEX IF NOT EXISTS idx_tasks_doc_id     ON tasks(doc_id);
        CREATE INDEX IF NOT EXISTS idx_tasks_created_at ON tasks(created_at);
    )";
    if (ExecSQL(db_, sql) != SQLITE_OK) {
        return F42Status(F42ErrorCode::kStorageFailed, "create tasks table");
    }
    // Idempotent migration for tasks.db created before metadata_json existed.
    // Gate on pragma_table_info: fresh DBs already have the column from CREATE
    // above, and existing migrated DBs have it too — running ALTER unconditionally
    // logged a spurious "duplicate column name" exec error on every startup.
    if (!ColumnExists(db_, "tasks", "metadata_json")) {
        ExecSQL(db_, "ALTER TABLE tasks ADD COLUMN metadata_json TEXT");
    }
    return Status::Ok();
}

// ---------------------------------------------------------------------------
// CRUD
// ---------------------------------------------------------------------------

Result<TaskInfo> TaskManager::CreateTask(TaskInfo task) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (task.task_id.empty()) task.task_id = id::GenerateUlid();
    const std::string now = NowIso8601();
    task.created_at = now;
    task.updated_at = now;

    const char* sql =
        "INSERT INTO tasks "
        "(task_id, namespace_id, filename, filepath, doc_id, content_hash, "
        " status, task_type, cancel_requested, total_pages, processed_pages, "
        " failed_pages, progress_pct, eta_seconds, current_phase, worker_id, "
        " trace_id, error_code, error_msg, structured_data, created_at, updated_at, "
        " started_at, completed_at, metadata_json) "
        "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return F42Status(F42ErrorCode::kStorageFailed,
                         "CreateTask prepare: " + std::string(sqlite3_errmsg(db_)));
    }
    BindText(stmt, 1, task.task_id);
    BindText(stmt, 2, task.namespace_id);
    BindText(stmt, 3, task.filename);
    BindText(stmt, 4, task.filepath);
    BindNullableText(stmt, 5, task.doc_id);
    BindNullableText(stmt, 6, task.content_hash);
    BindText(stmt, 7, task.status);
    sqlite3_bind_int(stmt, 8, task.task_type);
    sqlite3_bind_int(stmt, 9, task.cancel_requested ? 1 : 0);
    sqlite3_bind_int(stmt, 10, task.total_pages);
    sqlite3_bind_int(stmt, 11, task.processed_pages);
    BindText(stmt, 12, FailedPagesToJson(task.failed_pages));
    sqlite3_bind_double(stmt, 13, task.progress_pct);
    sqlite3_bind_int(stmt, 14, task.eta_seconds);
    BindNullableText(stmt, 15, task.current_phase);
    if (task.worker_id < 0) sqlite3_bind_null(stmt, 16);
    else sqlite3_bind_int(stmt, 16, task.worker_id);
    BindNullableText(stmt, 17, task.trace_id);
    BindNullableText(stmt, 18, task.error_code);
    BindNullableText(stmt, 19, task.error_msg);
    BindNullableText(stmt, 20, task.structured_data);
    BindText(stmt, 21, task.created_at);
    BindText(stmt, 22, task.updated_at);
    BindNullableText(stmt, 23, task.started_at);
    BindNullableText(stmt, 24, task.completed_at);
    BindNullableText(stmt, 25, task.metadata_json);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        return F42Status(F42ErrorCode::kStorageFailed,
                         "CreateTask step: " + std::string(sqlite3_errmsg(db_)));
    }
    return task;
}

Result<TaskInfo> TaskManager::GetTask(const std::string& task_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string sql = std::string("SELECT ") + kSelectColumns +
                      " FROM tasks WHERE task_id = ?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        return F42Status(F42ErrorCode::kStorageFailed, "GetTask prepare");
    }
    BindText(stmt, 1, task_id);
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        TaskInfo t = ReadRow(stmt);
        sqlite3_finalize(stmt);
        return t;
    }
    sqlite3_finalize(stmt);
    return F42Status(F42ErrorCode::kTaskNotFound, task_id);
}

Status TaskManager::UpdateProgress(const TaskInfo& task) {
    std::lock_guard<std::mutex> lock(mutex_);
    const char* sql =
        "UPDATE tasks SET processed_pages=?, total_pages=?, failed_pages=?, "
        "progress_pct=?, eta_seconds=?, current_phase=?, worker_id=?, "
        "updated_at=? WHERE task_id=?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return F42Status(F42ErrorCode::kStorageFailed, "UpdateProgress prepare");
    }
    sqlite3_bind_int(stmt, 1, task.processed_pages);
    sqlite3_bind_int(stmt, 2, task.total_pages);
    BindText(stmt, 3, FailedPagesToJson(task.failed_pages));
    sqlite3_bind_double(stmt, 4, task.progress_pct);
    sqlite3_bind_int(stmt, 5, task.eta_seconds);
    BindNullableText(stmt, 6, task.current_phase);
    if (task.worker_id < 0) sqlite3_bind_null(stmt, 7);
    else sqlite3_bind_int(stmt, 7, task.worker_id);
    BindText(stmt, 8, NowIso8601());
    BindText(stmt, 9, task.task_id);
    int rc = sqlite3_step(stmt);
    int changes = sqlite3_changes(db_);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        return F42Status(F42ErrorCode::kStorageFailed, "UpdateProgress step");
    }
    if (changes == 0) return F42Status(F42ErrorCode::kTaskNotFound, task.task_id);
    return Status::Ok();
}

Result<std::vector<TaskInfo>> TaskManager::ListByNamespace(
    const std::string& namespace_id, int limit, int offset) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string sql = std::string("SELECT ") + kSelectColumns +
                      " FROM tasks WHERE namespace_id = ? "
                      "ORDER BY created_at DESC LIMIT ? OFFSET ?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        return F42Status(F42ErrorCode::kStorageFailed, "ListByNamespace prepare");
    }
    BindText(stmt, 1, namespace_id);
    sqlite3_bind_int(stmt, 2, limit);
    sqlite3_bind_int(stmt, 3, offset);
    std::vector<TaskInfo> out;
    while (sqlite3_step(stmt) == SQLITE_ROW) out.push_back(ReadRow(stmt));
    sqlite3_finalize(stmt);
    return out;
}

// ---------------------------------------------------------------------------
// State transitions
// ---------------------------------------------------------------------------

Status TaskManager::MarkProcessing(const std::string& task_id, int worker_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const char* sql =
        "UPDATE tasks SET status='processing', worker_id=?, started_at=?, "
        "updated_at=? WHERE task_id=?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return F42Status(F42ErrorCode::kStorageFailed, "MarkProcessing prepare");
    }
    const std::string now = NowIso8601();
    sqlite3_bind_int(stmt, 1, worker_id);
    BindText(stmt, 2, now);
    BindText(stmt, 3, now);
    BindText(stmt, 4, task_id);
    int rc = sqlite3_step(stmt);
    int changes = sqlite3_changes(db_);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return F42Status(F42ErrorCode::kStorageFailed, "MarkProcessing step");
    if (changes == 0) return F42Status(F42ErrorCode::kTaskNotFound, task_id);
    return Status::Ok();
}

Status TaskManager::MarkCompleted(const std::string& task_id,
                                  const std::string& doc_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const char* sql =
        "UPDATE tasks SET status='completed', doc_id=?, progress_pct=100.0, "
        "completed_at=?, updated_at=? WHERE task_id=?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return F42Status(F42ErrorCode::kStorageFailed, "MarkCompleted prepare");
    }
    const std::string now = NowIso8601();
    BindNullableText(stmt, 1, doc_id);
    BindText(stmt, 2, now);
    BindText(stmt, 3, now);
    BindText(stmt, 4, task_id);
    int rc = sqlite3_step(stmt);
    int changes = sqlite3_changes(db_);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return F42Status(F42ErrorCode::kStorageFailed, "MarkCompleted step");
    if (changes == 0) return F42Status(F42ErrorCode::kTaskNotFound, task_id);
    return Status::Ok();
}

Status TaskManager::MarkFailed(const std::string& task_id,
                               const std::string& error_code,
                               const std::string& error_msg,
                               const std::string& structured_data) {
    std::lock_guard<std::mutex> lock(mutex_);
    const char* sql =
        "UPDATE tasks SET status='failed', error_code=?, error_msg=?, "
        "structured_data=?, completed_at=?, updated_at=? WHERE task_id=?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return F42Status(F42ErrorCode::kStorageFailed, "MarkFailed prepare");
    }
    const std::string now = NowIso8601();
    BindNullableText(stmt, 1, error_code);
    BindNullableText(stmt, 2, error_msg);
    BindNullableText(stmt, 3, structured_data);
    BindText(stmt, 4, now);
    BindText(stmt, 5, now);
    BindText(stmt, 6, task_id);
    int rc = sqlite3_step(stmt);
    int changes = sqlite3_changes(db_);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return F42Status(F42ErrorCode::kStorageFailed, "MarkFailed step");
    if (changes == 0) return F42Status(F42ErrorCode::kTaskNotFound, task_id);
    return Status::Ok();
}

Status TaskManager::RequestCancel(const std::string& task_id, TaskInfo* out) {
    // Read-modify-write under the lock: the status transition depends on the
    // current status (topic 3 — queued → cancelled, processing → cancelling,
    // already cancelling/terminal → 423 TASK_CANCELLING).
    std::lock_guard<std::mutex> lock(mutex_);

    std::string sel = std::string("SELECT ") + kSelectColumns +
                      " FROM tasks WHERE task_id = ?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sel.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        return F42Status(F42ErrorCode::kStorageFailed, "RequestCancel select prepare");
    }
    BindText(stmt, 1, task_id);
    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return F42Status(F42ErrorCode::kTaskNotFound, task_id);
    }
    TaskInfo t = ReadRow(stmt);
    sqlite3_finalize(stmt);

    std::string new_status;
    if (t.status == task_status::kQueued) {
        new_status = task_status::kCancelled;  // never started → terminal directly
    } else if (t.status == task_status::kProcessing) {
        new_status = task_status::kCancelling;  // signal the worker checkpoint
    } else {
        // cancelling / completed / failed / cancelled → repeat-cancel is a 423.
        return F42Status(F42ErrorCode::kTaskCancelling,
                         "task " + task_id + " status=" + t.status);
    }

    const std::string now = NowIso8601();
    const bool terminal = (new_status == task_status::kCancelled);
    const char* upd =
        terminal
            ? "UPDATE tasks SET cancel_requested=1, status=?, completed_at=?, "
              "updated_at=? WHERE task_id=?"
            : "UPDATE tasks SET cancel_requested=1, status=?, updated_at=? "
              "WHERE task_id=?";
    sqlite3_stmt* us = nullptr;
    if (sqlite3_prepare_v2(db_, upd, -1, &us, nullptr) != SQLITE_OK) {
        return F42Status(F42ErrorCode::kStorageFailed, "RequestCancel update prepare");
    }
    BindText(us, 1, new_status);
    if (terminal) {
        BindText(us, 2, now);
        BindText(us, 3, now);
        BindText(us, 4, task_id);
    } else {
        BindText(us, 2, now);
        BindText(us, 3, task_id);
    }
    int rc = sqlite3_step(us);
    sqlite3_finalize(us);
    if (rc != SQLITE_DONE) return F42Status(F42ErrorCode::kStorageFailed, "RequestCancel step");

    if (out) {
        t.cancel_requested = true;
        t.status = new_status;
        t.updated_at = now;
        if (terminal) t.completed_at = now;
        *out = t;
    }
    return Status::Ok();
}

Status TaskManager::MarkCancelled(const std::string& task_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const char* sql =
        "UPDATE tasks SET status='cancelled', completed_at=?, updated_at=? "
        "WHERE task_id=?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return F42Status(F42ErrorCode::kStorageFailed, "MarkCancelled prepare");
    }
    const std::string now = NowIso8601();
    BindText(stmt, 1, now);
    BindText(stmt, 2, now);
    BindText(stmt, 3, task_id);
    int rc = sqlite3_step(stmt);
    int changes = sqlite3_changes(db_);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return F42Status(F42ErrorCode::kStorageFailed, "MarkCancelled step");
    if (changes == 0) return F42Status(F42ErrorCode::kTaskNotFound, task_id);
    return Status::Ok();
}

// ---------------------------------------------------------------------------
// Scheduler-support queries
// ---------------------------------------------------------------------------

Result<std::optional<TaskInfo>> TaskManager::FindRecentTaskByDocId(
    const std::string& doc_id, int task_type, int window_seconds) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (doc_id.empty()) return std::optional<TaskInfo>{};

    auto now = std::chrono::system_clock::now();
    int64_t now_secs = std::chrono::duration_cast<std::chrono::seconds>(
                           now.time_since_epoch()).count();
    const std::string cutoff = UnixToIso8601(now_secs - window_seconds);

    std::string sql = std::string("SELECT ") + kSelectColumns +
                      " FROM tasks WHERE doc_id = ? AND task_type = ? AND created_at >= ? "
                      "ORDER BY created_at DESC LIMIT 1";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        return F42Status(F42ErrorCode::kStorageFailed, "FindRecentTaskByDocId prepare");
    }
    BindText(stmt, 1, doc_id);
    sqlite3_bind_int(stmt, 2, task_type);
    BindText(stmt, 3, cutoff);
    std::optional<TaskInfo> out;
    if (sqlite3_step(stmt) == SQLITE_ROW) out = ReadRow(stmt);
    sqlite3_finalize(stmt);
    return out;
}

Result<TaskInfo> TaskManager::UpdateTaskForDebounce(const std::string& task_id,
                                                    const SubmitRequest& req) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const char* sql =
            "UPDATE tasks SET content_hash=?, filepath=?, total_pages=?, "
            "status='queued', processed_pages=0, failed_pages='[]', "
            "progress_pct=0.0, eta_seconds=-1, current_phase=NULL, "
            "worker_id=NULL, error_code=NULL, error_msg=NULL, "
            "structured_data=NULL, started_at=NULL, completed_at=NULL, "
            "cancel_requested=0, updated_at=? WHERE task_id=?";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            return F42Status(F42ErrorCode::kStorageFailed, "UpdateTaskForDebounce prepare");
        }
        BindNullableText(stmt, 1, req.content_hash);
        BindText(stmt, 2, req.filepath);
        sqlite3_bind_int(stmt, 3, req.total_pages);
        BindText(stmt, 4, NowIso8601());
        BindText(stmt, 5, task_id);
        int rc = sqlite3_step(stmt);
        int changes = sqlite3_changes(db_);
        sqlite3_finalize(stmt);
        if (rc != SQLITE_DONE) return F42Status(F42ErrorCode::kStorageFailed, "UpdateTaskForDebounce step");
        if (changes == 0) return F42Status(F42ErrorCode::kTaskNotFound, task_id);
    }
    return GetTask(task_id);
}

Result<std::optional<TaskInfo>> TaskManager::SelectOldestQueuedTaskExcluding(
    const std::vector<std::string>& active_doc_ids) {
    std::lock_guard<std::mutex> lock(mutex_);
    // Build "doc_id IS NULL OR doc_id NOT IN (?,?,...)" so queued rows whose
    // doc_id is currently being processed are skipped (topic 2.3 per-doc_id mutex).
    // A NULL doc_id (Phase 1 corner) is never "active", so it stays eligible.
    std::string sql = std::string("SELECT ") + kSelectColumns +
                      " FROM tasks WHERE status='queued'";
    if (!active_doc_ids.empty()) {
        sql += " AND (doc_id IS NULL OR doc_id NOT IN (";
        for (size_t i = 0; i < active_doc_ids.size(); ++i) {
            sql += (i == 0) ? "?" : ",?";
        }
        sql += "))";
    }
    sql += " ORDER BY created_at ASC LIMIT 1";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        return F42Status(F42ErrorCode::kStorageFailed,
                         "SelectOldestQueuedTaskExcluding prepare");
    }
    for (size_t i = 0; i < active_doc_ids.size(); ++i) {
        BindText(stmt, static_cast<int>(i) + 1, active_doc_ids[i]);
    }
    std::optional<TaskInfo> out;
    if (sqlite3_step(stmt) == SQLITE_ROW) out = ReadRow(stmt);
    sqlite3_finalize(stmt);
    return out;
}

// ---------------------------------------------------------------------------
// Cleanup (topic 4)
// ---------------------------------------------------------------------------

Result<int> TaskManager::DeleteExpired(int64_t now_unix, int retention_days) {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string cutoff =
        UnixToIso8601(now_unix - static_cast<int64_t>(retention_days) * 86400);
    const char* sql =
        "DELETE FROM tasks WHERE status IN ('completed','failed','cancelled') "
        "AND updated_at < ?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return F42Status(F42ErrorCode::kStorageFailed, "DeleteExpired prepare");
    }
    BindText(stmt, 1, cutoff);
    int rc = sqlite3_step(stmt);
    int changes = sqlite3_changes(db_);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return F42Status(F42ErrorCode::kStorageFailed, "DeleteExpired step");
    return changes;
}

Result<int> TaskManager::SweepZombies(int64_t now_unix, int zombie_hours) {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string cutoff =
        UnixToIso8601(now_unix - static_cast<int64_t>(zombie_hours) * 3600);
    const char* sql =
        "UPDATE tasks SET status='failed', error_code='CX_ERR_ZOMBIE_TASK_CLEANUP', "
        "error_msg='Worker presumed crashed.', completed_at=?, updated_at=? "
        "WHERE status='processing' AND updated_at < ?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return F42Status(F42ErrorCode::kStorageFailed, "SweepZombies prepare");
    }
    const std::string now = NowIso8601();
    BindText(stmt, 1, now);
    BindText(stmt, 2, now);
    BindText(stmt, 3, cutoff);
    int rc = sqlite3_step(stmt);
    int changes = sqlite3_changes(db_);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return F42Status(F42ErrorCode::kStorageFailed, "SweepZombies step");
    return changes;
}

Result<int> TaskManager::SweepTimedOut(int64_t now_unix, int timeout_seconds) {
    std::lock_guard<std::mutex> lock(mutex_);
    // §6.1 / §7 v0 — processing tasks that started more than `timeout_seconds` ago
    // exceeded the budget → failed + CX_ERR_TASK_TIMEOUT. Keyed on started_at (not
    // updated_at) so an actively-progressing-but-too-long task is still caught.
    const std::string cutoff =
        UnixToIso8601(now_unix - static_cast<int64_t>(timeout_seconds));
    const char* sql =
        "UPDATE tasks SET status='failed', error_code='CX_ERR_TASK_TIMEOUT', "
        "error_msg='Task exceeded processing timeout.', completed_at=?, updated_at=? "
        "WHERE status='processing' AND started_at IS NOT NULL AND started_at < ?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return F42Status(F42ErrorCode::kStorageFailed, "SweepTimedOut prepare");
    }
    const std::string now = NowIso8601();
    BindText(stmt, 1, now);
    BindText(stmt, 2, now);
    BindText(stmt, 3, cutoff);
    int rc = sqlite3_step(stmt);
    int changes = sqlite3_changes(db_);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return F42Status(F42ErrorCode::kStorageFailed, "SweepTimedOut step");
    return changes;
}

Result<int> TaskManager::RequeueStaleProcessing(int64_t now_unix, int zombie_hours) {
    std::lock_guard<std::mutex> lock(mutex_);
    // §6.1 crash recovery: processing rows younger than the zombie threshold are
    // re-queued on restart (worker crashed but the doc is still worth retrying);
    // rows older than the threshold are left for SweepZombies → failed.
    const std::string cutoff =
        UnixToIso8601(now_unix - static_cast<int64_t>(zombie_hours) * 3600);
    const char* sql =
        "UPDATE tasks SET status='queued', worker_id=NULL, started_at=NULL, "
        "updated_at=? WHERE status='processing' AND updated_at >= ?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return F42Status(F42ErrorCode::kStorageFailed, "RequeueStaleProcessing prepare");
    }
    BindText(stmt, 1, NowIso8601());
    BindText(stmt, 2, cutoff);
    int rc = sqlite3_step(stmt);
    int changes = sqlite3_changes(db_);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return F42Status(F42ErrorCode::kStorageFailed, "RequeueStaleProcessing step");
    return changes;
}

Result<int> TaskManager::CountAll() {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM tasks", -1, &stmt, nullptr) != SQLITE_OK) {
        return F42Status(F42ErrorCode::kStorageFailed, "CountAll prepare");
    }
    int n = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) n = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return n;
}

}  // namespace cortrix::async
