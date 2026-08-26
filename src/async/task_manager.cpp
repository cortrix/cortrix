#include <cstdint>
#include "cortrix/async/task_manager.h"

#include <sqlite3.h>

#include <chrono>
#include <filesystem>
#include <cstdio>
#include <ctime>
#include <utility>
#include <vector>

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
            -- filepath resolved once at task creation (weakly_canonical). The
            -- managed-input reaper needs "does any other live task still need this
            -- file", and rows spell the same file differently ("d/x", "d/./x"), so
            -- the comparison has to happen on resolved paths. Resolving them at
            -- creation makes that an indexed point lookup instead of resolving every
            -- live row on every task completion (issue #74).
            filepath_canonical TEXT,
            CHECK (status IN ('queued','processing','cancelling','completed','failed','cancelled'))
        );
        CREATE INDEX IF NOT EXISTS idx_tasks_namespace  ON tasks(namespace_id);
        CREATE INDEX IF NOT EXISTS idx_tasks_status     ON tasks(status);
        CREATE INDEX IF NOT EXISTS idx_tasks_doc_id     ON tasks(doc_id);
        -- Debounce identity is (namespace_id, doc_id, task_type); keep it indexed
        -- so the per-submit lookup stays a point query as the table grows.
        CREATE INDEX IF NOT EXISTS idx_tasks_ns_doc_type ON tasks(namespace_id, doc_id, task_type, created_at);
        CREATE INDEX IF NOT EXISTS idx_tasks_created_at ON tasks(created_at);
        -- Dispatch access pattern (SelectOldestQueuedTaskExcluding): filter on
        -- status='queued', order by created_at, LIMIT 1 — issued by every worker on
        -- every dequeue attempt while the TaskManager mutex is held. The separate
        -- status / created_at indexes cannot serve both halves, so SQLite either
        -- scans the queued rows or sorts them; at a deep queue that scan is O(queued)
        -- per dequeue and gates the whole worker pool (issue #72: in-flight tasks
        -- stayed at 2-5 regardless of worker count at ~71k queued). The composite
        -- index satisfies the filter and the ordering, turning dispatch into a point
        -- lookup. CREATE INDEX IF NOT EXISTS runs on every Init, so this doubles as
        -- the migration for tasks.db files created before the index existed.
        CREATE INDEX IF NOT EXISTS idx_tasks_status_created ON tasks(status, created_at);
        -- Managed-input reference check (issue #74): "is this resolved path still
        -- referenced by a live task other than the one releasing it". Indexed so
        -- that check is a point lookup; NULLs are indexed too, which is what the
        -- un-backfilled-row guard below probes.
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
    // Same idempotent shape for filepath_canonical. ALTER cannot backfill it —
    // resolving a path needs the filesystem — so rows written by an older binary
    // arrive here with NULL. BackfillCanonicalInputsLocked() fills the live ones;
    // any that remain NULL are treated as "could be this file" by the reference
    // check, which keeps the reaper failing closed on a partially migrated DB.
    if (!ColumnExists(db_, "tasks", "filepath_canonical")) {
        ExecSQL(db_, "ALTER TABLE tasks ADD COLUMN filepath_canonical TEXT");
    }
    // Only now is the column guaranteed to exist. Indexing it inside the CREATE
    // batch above would be correct on a fresh database and fatal on an existing
    // one: CREATE TABLE IF NOT EXISTS leaves the old table alone, so the index
    // statement hits "no such column", the whole batch reports failure, and Init
    // fails before the ALTER that would have added the column ever runs.
    // Partial index, restricted to live rows. A full index on filepath_canonical is
    // useless here and actively harmful: every historical row carries NULL (the
    // backfill only resolves live ones, and terminal rows have already released
    // their input), so the "is any live identity still unknown" probe walks every
    // NULL entry in the table before status can rule it out. Measured on a
    // 801k-row tasks.db with 722k NULLs: 1.109s per probe with the full index,
    // 0.003s with this one -- and the full-index cost grows with total history,
    // which is worse than the scan it replaced.
    ExecSQL(db_, "DROP INDEX IF EXISTS idx_tasks_filepath_canonical");
    ExecSQL(db_,
            "CREATE INDEX IF NOT EXISTS idx_tasks_live_canonical "
            "ON tasks(filepath_canonical) "
            "WHERE status NOT IN ('completed','failed','cancelled')");
    BackfillCanonicalInputsLocked();
    return Status::Ok();
}

void TaskManager::BackfillCanonicalInputsLocked() {
    // Only live rows matter: a terminal task's input has already been released, and
    // resolving thousands of historical paths on every start would be pure cost.
    const char* select_sql =
        "SELECT task_id, filepath FROM tasks "
        "WHERE status NOT IN ('completed','failed','cancelled') "
        "AND filepath IS NOT NULL AND filepath <> '' "
        "AND (filepath_canonical IS NULL OR filepath_canonical = '')";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, select_sql, -1, &stmt, nullptr) != SQLITE_OK) {
        spdlog::warn("F42 filepath_canonical backfill skipped: {}", sqlite3_errmsg(db_));
        return;
    }
    std::vector<std::pair<std::string, std::string>> pending;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        pending.emplace_back(SafeColText(stmt, 0), SafeColText(stmt, 1));
    }
    sqlite3_finalize(stmt);
    if (pending.empty()) return;

    const char* update_sql = "UPDATE tasks SET filepath_canonical = ? WHERE task_id = ?";
    int filled = 0;
    for (const auto& [task_id, filepath] : pending) {
        std::error_code ec;
        const std::string canonical = std::filesystem::weakly_canonical(filepath, ec).string();
        // An unresolvable path stays NULL rather than getting a guessed value: the
        // reference check reads NULL as "might be this file" and declines to delete.
        if (ec || canonical.empty()) continue;
        sqlite3_stmt* up = nullptr;
        if (sqlite3_prepare_v2(db_, update_sql, -1, &up, nullptr) != SQLITE_OK) continue;
        BindText(up, 1, canonical);
        BindText(up, 2, task_id);
        if (sqlite3_step(up) == SQLITE_DONE) ++filled;
        sqlite3_finalize(up);
    }
    spdlog::info("F42 filepath_canonical backfill: {}/{} live task input(s) resolved",
                 filled, pending.size());
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
        " started_at, completed_at, metadata_json, filepath_canonical) "
        "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)";

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
    // Resolve the input path once, here, so the managed-input reaper can compare
    // resolved identities with a point lookup instead of resolving every live row
    // on every completion (issue #74). Unresolvable → NULL, which the reference
    // check reads as "might be this file" and refuses to delete on.
    {
        std::string canonical;
        if (!task.filepath.empty()) {
            std::error_code ec;
            canonical = std::filesystem::weakly_canonical(task.filepath, ec).string();
            if (ec) canonical.clear();
        }
        BindNullableText(stmt, 26, canonical);
    }

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
    // Conditional on the row still being queued. Dequeue selects a row and only
    // then marks it, so a cancel (queued -> cancelled, which also releases the
    // input) can land in between. An unconditional update would resurrect that row
    // to processing and dispatch a worker whose input no longer exists.
    const char* sql =
        "UPDATE tasks SET status='processing', worker_id=?, started_at=?, "
        "updated_at=? WHERE task_id=? AND status='queued'";
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
    if (changes == 0) {
        // 0 rows = the row is gone, OR it left `queued` under us (cancelled, or
        // claimed by another worker). Distinguish them: a missing row keeps the
        // established CX_ERR_TASK_NOT_FOUND contract, while a live row that simply
        // moved on is a transient conflict the caller retries on its next tick.
        sqlite3_stmt* probe = nullptr;
        bool exists = false;
        if (sqlite3_prepare_v2(db_, "SELECT 1 FROM tasks WHERE task_id=?", -1, &probe,
                               nullptr) == SQLITE_OK) {
            BindText(probe, 1, task_id);
            exists = (sqlite3_step(probe) == SQLITE_ROW);
            sqlite3_finalize(probe);
        }
        return exists ? F42Status(F42ErrorCode::kDocProcessingInProgress, task_id)
                      : F42Status(F42ErrorCode::kTaskNotFound, task_id);
    }
    return Status::Ok();
}

Status TaskManager::RejectedTransitionStatus(const std::string& task_id,
                                             const char* target) {
    // 0 rows changed on a guarded terminal transition: distinguish a missing row
    // (keeps the established CX_ERR_TASK_NOT_FOUND contract) from a live row
    // whose current status forbids the transition — same conflict identity
    // MarkProcessing uses. Called with mutex_ held.
    sqlite3_stmt* probe = nullptr;
    std::string current;
    if (sqlite3_prepare_v2(db_, "SELECT status FROM tasks WHERE task_id=?", -1,
                           &probe, nullptr) == SQLITE_OK) {
        BindText(probe, 1, task_id);
        if (sqlite3_step(probe) == SQLITE_ROW) {
            const unsigned char* s = sqlite3_column_text(probe, 0);
            current = s ? reinterpret_cast<const char*>(s) : "";
        }
        sqlite3_finalize(probe);
    }
    if (current.empty()) return F42Status(F42ErrorCode::kTaskNotFound, task_id);
    spdlog::warn(
        "F42 TaskManager: rejected illegal transition task_id={} {} -> {} "
        "(terminal states are immutable)",
        task_id, current, target);
    return F42Status(F42ErrorCode::kDocProcessingInProgress,
                     "task " + task_id + " status=" + current +
                         " cannot transition to " + target);
}

Status TaskManager::MarkCompleted(const std::string& task_id,
                                  const std::string& doc_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    // Legal sources only (same guard family as MarkProcessing, fixed for the
    // terminal transitions after the #32 audit / issue #57): `processing` is the
    // normal path; `cancelling` is the late-cancel race — the cancel request
    // landed after the worker's last checkpoint, the work IS done, and the
    // best-effort cancel loses (rejecting here would strand the row in
    // `cancelling` forever and leak its managed input). Terminal rows
    // (completed/failed/cancelled) and `queued` are never overwritten.
    const char* sql =
        "UPDATE tasks SET status='completed', doc_id=?, progress_pct=100.0, "
        "completed_at=?, updated_at=? "
        "WHERE task_id=? AND status IN ('processing','cancelling')";
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
    if (changes == 0) return RejectedTransitionStatus(task_id, "completed");
    return Status::Ok();
}

Status TaskManager::MarkFailed(const std::string& task_id,
                               const std::string& error_code,
                               const std::string& error_msg,
                               const std::string& structured_data) {
    std::lock_guard<std::mutex> lock(mutex_);
    // Same legal-source guard as MarkCompleted: `processing` (normal) or
    // `cancelling` (worker errored after a late cancel — the honest terminal is
    // `failed` with the error preserved; rejecting would strand the row).
    const char* sql =
        "UPDATE tasks SET status='failed', error_code=?, error_msg=?, "
        "structured_data=?, completed_at=?, updated_at=? "
        "WHERE task_id=? AND status IN ('processing','cancelling')";
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
    if (changes == 0) return RejectedTransitionStatus(task_id, "failed");
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
    // Legal sources = the two state-machine edges into `cancelled`: `queued`
    // (a never-started task) and `cancelling` (the worker acknowledged the
    // cancel checkpoint). A completed/failed row is immutable — a late cancel
    // must not overwrite it, and a `processing` row must go through
    // RequestCancel's processing -> cancelling edge first.
    const char* sql =
        "UPDATE tasks SET status='cancelled', completed_at=?, updated_at=? "
        "WHERE task_id=? AND status IN ('queued','cancelling')";
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
    if (changes == 0) return RejectedTransitionStatus(task_id, "cancelled");
    return Status::Ok();
}

// ---------------------------------------------------------------------------
// Scheduler-support queries
// ---------------------------------------------------------------------------

Result<std::optional<TaskInfo>> TaskManager::FindRecentTaskByDocId(
    const std::string& namespace_id, const std::string& doc_id, int task_type,
    int window_seconds) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (doc_id.empty()) return std::optional<TaskInfo>{};

    auto now = std::chrono::system_clock::now();
    int64_t now_secs = std::chrono::duration_cast<std::chrono::seconds>(
                           now.time_since_epoch()).count();
    const std::string cutoff = UnixToIso8601(now_secs - window_seconds);

    std::string sql = std::string("SELECT ") + kSelectColumns +
                      " FROM tasks WHERE namespace_id = ? AND doc_id = ? "
                      "AND task_type = ? AND created_at >= ? "
                      // Only a task that will actually carry the submission is a
                      // debounce candidate. Merging into a cancelled, cancelling or
                      // failed task swallows the resubmission: the caller is handed
                      // that task_id and its input is released, but nothing will ever
                      // process the content. (`completed` stays eligible — identical
                      // content already ingested is exactly what dedup should report.)
                      "AND status NOT IN ('cancelled','cancelling','failed') "
                      "ORDER BY created_at DESC LIMIT 1";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        return F42Status(F42ErrorCode::kStorageFailed, "FindRecentTaskByDocId prepare");
    }
    BindText(stmt, 1, namespace_id);
    BindText(stmt, 2, doc_id);
    sqlite3_bind_int(stmt, 3, task_type);
    BindText(stmt, 4, cutoff);
    std::optional<TaskInfo> out;
    if (sqlite3_step(stmt) == SQLITE_ROW) out = ReadRow(stmt);
    sqlite3_finalize(stmt);
    return out;
}

Result<TaskInfo> TaskManager::TryClaimDebounceMerge(const std::string& task_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    // Claim with a write so the merge is a real linearization point rather than a
    // read whose value may already be stale by the time the caller acts on it. The
    // touch and the read-back happen under the one lock RequestCancel also takes.
    const char* claim =
        "UPDATE tasks SET updated_at=? WHERE task_id=? "
        "AND status NOT IN ('cancelled','cancelling','failed')";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, claim, -1, &stmt, nullptr) != SQLITE_OK) {
        return F42Status(F42ErrorCode::kStorageFailed, "TryClaimDebounceMerge prepare");
    }
    BindText(stmt, 1, NowIso8601());
    BindText(stmt, 2, task_id);
    const int rc = sqlite3_step(stmt);
    const int changes = sqlite3_changes(db_);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        return F42Status(F42ErrorCode::kStorageFailed, "TryClaimDebounceMerge step");
    }
    if (changes == 0) {
        // Either the row went terminal under us, or it is gone. Both mean the same
        // thing to the caller: do not merge into it.
        sqlite3_stmt* probe = nullptr;
        bool exists = false;
        if (sqlite3_prepare_v2(db_, "SELECT 1 FROM tasks WHERE task_id=?", -1, &probe,
                               nullptr) == SQLITE_OK) {
            BindText(probe, 1, task_id);
            exists = (sqlite3_step(probe) == SQLITE_ROW);
            sqlite3_finalize(probe);
        }
        return exists ? F42Status(F42ErrorCode::kDocProcessingInProgress, task_id)
                      : F42Status(F42ErrorCode::kTaskNotFound, task_id);
    }
    // Read back under the same lock so the returned row reflects the claimed state.
    std::string sel = std::string("SELECT ") + kSelectColumns + " FROM tasks WHERE task_id = ?";
    sqlite3_stmt* rd = nullptr;
    if (sqlite3_prepare_v2(db_, sel.c_str(), -1, &rd, nullptr) != SQLITE_OK) {
        return F42Status(F42ErrorCode::kStorageFailed, "TryClaimDebounceMerge select");
    }
    BindText(rd, 1, task_id);
    if (sqlite3_step(rd) != SQLITE_ROW) {
        sqlite3_finalize(rd);
        return F42Status(F42ErrorCode::kTaskNotFound, task_id);
    }
    TaskInfo out = ReadRow(rd);
    sqlite3_finalize(rd);
    return out;
}

Result<TaskInfo> TaskManager::UpdateTaskForDebounce(const std::string& task_id,
                                                    const SubmitRequest& req) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // Conditional on the row still being queued: this is the authority for the
        // debounce decision, not the status the caller read a moment earlier. A
        // cancel (queued -> cancelled, which also releases the input) does not share
        // the scheduler mutex, so it can land between that read and this write. An
        // unconditional update would resurrect the cancelled row under the same
        // task_id for a different submission, after its cancel response already told
        // the caller the task was terminal.
        const char* sql =
            "UPDATE tasks SET content_hash=?, filepath=?, total_pages=?, "
            "status='queued', processed_pages=0, failed_pages='[]', "
            "progress_pct=0.0, eta_seconds=-1, current_phase=NULL, "
            "worker_id=NULL, error_code=NULL, error_msg=NULL, "
            "structured_data=NULL, started_at=NULL, completed_at=NULL, "
            "cancel_requested=0, updated_at=? WHERE task_id=? AND status='queued'";
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
        if (changes == 0) {
            // Distinguish "row is gone" from "row left queued under us"; the caller
            // turns the latter into a separate task for the new submission.
            sqlite3_stmt* probe = nullptr;
            bool exists = false;
            if (sqlite3_prepare_v2(db_, "SELECT 1 FROM tasks WHERE task_id=?", -1,
                                   &probe, nullptr) == SQLITE_OK) {
                BindText(probe, 1, task_id);
                exists = (sqlite3_step(probe) == SQLITE_ROW);
                sqlite3_finalize(probe);
            }
            return exists ? F42Status(F42ErrorCode::kDocProcessingInProgress, task_id)
                          : F42Status(F42ErrorCode::kTaskNotFound, task_id);
        }
    }
    return GetTask(task_id);
}

Result<std::optional<TaskInfo>> TaskManager::SelectOldestQueuedTaskExcluding(
    const std::vector<std::pair<std::string, std::string>>& active_docs) {
    std::lock_guard<std::mutex> lock(mutex_);
    // Skip queued rows whose (namespace_id, doc_id) is currently being processed
    // (topic 2.3, per-doc mutex). Excluding on the PAIR, not the bare doc_id, so
    // one namespace's in-flight document cannot block another namespace's document
    // that merely shares a caller-chosen id. A NULL doc_id (Phase 1 corner) is
    // never "active", so it stays eligible.
    std::string sql = std::string("SELECT ") + kSelectColumns +
                      " FROM tasks WHERE status='queued'";
    for (size_t i = 0; i < active_docs.size(); ++i) {
        sql += " AND (doc_id IS NULL OR NOT (namespace_id = ? AND doc_id = ?))";
    }
    sql += " ORDER BY created_at ASC LIMIT 1";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        return F42Status(F42ErrorCode::kStorageFailed,
                         "SelectOldestQueuedTaskExcluding prepare");
    }
    for (size_t i = 0; i < active_docs.size(); ++i) {
        BindText(stmt, static_cast<int>(i) * 2 + 1, active_docs[i].first);
        BindText(stmt, static_cast<int>(i) * 2 + 2, active_docs[i].second);
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

Result<bool> TaskManager::ReleaseInputIfUnreferenced(
    const std::string& canonical_path, const std::string& exclude_task_id,
    const std::function<bool()>& release) {
    if (canonical_path.empty() || !release) return false;
    std::lock_guard<std::mutex> lock(mutex_);

    // Two point lookups instead of resolving every live row (issue #74). Both are
    // served by idx_tasks_filepath_canonical.
    //
    // 1. Does another live task name this exact resolved path?
    const char* referenced_sql =
        "SELECT 1 FROM tasks "
        "WHERE filepath_canonical = ? AND task_id <> ? "
        "AND status NOT IN ('completed','failed','cancelled') LIMIT 1";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, referenced_sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return F42Status(F42ErrorCode::kStorageFailed, "ReleaseInputIfUnreferenced prepare");
    }
    BindText(stmt, 1, canonical_path);
    BindText(stmt, 2, exclude_task_id);
    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc == SQLITE_ROW) return false;  // still referenced
    if (rc != SQLITE_DONE) {
        return F42Status(F42ErrorCode::kStorageFailed, "ReleaseInputIfUnreferenced step");
    }

    // 2. Is any live row's identity unknown? A row whose path could not be resolved
    // (older binary, or a path that failed to canonicalize at creation) carries NULL
    // here, so the lookup above cannot rule it out — treat it as "might be this file"
    // and keep the input. Fail closed, exactly as the full scan did.
    // IS NULL only: CreateTask binds empty as SQL NULL, so '' never reaches the
    // column. Spelling it as one predicate keeps this a single indexed search
    // against idx_tasks_live_canonical instead of a two-branch OR.
    const char* unresolved_sql =
        "SELECT 1 FROM tasks "
        "WHERE filepath_canonical IS NULL "
        "AND filepath IS NOT NULL AND filepath <> '' AND task_id <> ? "
        "AND status NOT IN ('completed','failed','cancelled') LIMIT 1";
    sqlite3_stmt* guard = nullptr;
    if (sqlite3_prepare_v2(db_, unresolved_sql, -1, &guard, nullptr) != SQLITE_OK) {
        return F42Status(F42ErrorCode::kStorageFailed, "ReleaseInputIfUnreferenced guard prepare");
    }
    BindText(guard, 1, exclude_task_id);
    const int grc = sqlite3_step(guard);
    sqlite3_finalize(guard);
    if (grc == SQLITE_ROW) return false;  // unknown identity in flight -> keep
    if (grc != SQLITE_DONE) {
        return F42Status(F42ErrorCode::kStorageFailed, "ReleaseInputIfUnreferenced guard step");
    }

    // Unreferenced, and still holding the lock: a CreateTask naming this path cannot
    // interleave between the decision and the removal.
    return release();
}

Result<std::vector<std::pair<std::string, std::string>>> TaskManager::LiveTaskInputs() {
    std::lock_guard<std::mutex> lock(mutex_);
    // Terminal statuses (completed/failed/cancelled) are excluded rather than
    // listing queued/processing explicitly: a future non-terminal status is then
    // preserved by default, so a reaper can never start deleting live inputs
    // because a new state was added.
    const char* sql =
        "SELECT task_id, filepath FROM tasks "
        "WHERE status NOT IN ('completed','failed','cancelled') "
        "AND filepath IS NOT NULL AND filepath <> ''";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return F42Status(F42ErrorCode::kStorageFailed, "LiveTaskInputs prepare");
    }
    std::vector<std::pair<std::string, std::string>> out;
    int rc = SQLITE_OK;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        out.emplace_back(SafeColText(stmt, 0), SafeColText(stmt, 1));
    }
    sqlite3_finalize(stmt);
    // Fail closed. A partial live set read as if it were complete would make the
    // reaper delete inputs that live tasks still need, so an interrupted scan is
    // an error rather than a shorter list.
    if (rc != SQLITE_DONE) {
        return F42Status(F42ErrorCode::kStorageFailed, "LiveTaskInputs step");
    }
    return out;
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
