#pragma once
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "cortrix/async/task_info.h"
#include "cortrix/common/result.h"
#include "cortrix/common/status.h"

struct sqlite3;

namespace cortrix::async {

/// Owns the `tasks` SQLite table (F42 §3.1) — the async-scheduling SoT. A
/// self-contained store (mirrors cortrix::MemoryStore): its own Init(db_path)
/// runs the DDL (CREATE TABLE IF NOT EXISTS + 4 indexes), and every CRUD method
/// is a prepared-statement op guarded by an internal mutex. Tasks metadata is
/// decoupled from the catalog blob_gc_queue (F42 §4.4), so this does NOT register
/// with the catalog SchemaMigrator.
///
/// Standalone (D3): the table + CRUD + cleanup are real and fully tested against
/// ":memory:". Cross-Feature wiring (real SPC pipeline, real server route
/// registration) is deferred to D3.5; TaskManager itself has no such dependency.
///
/// Thread-safe: all public methods serialize on mutex_. The owned sqlite3 handle
/// is opened in WAL mode with foreign_keys enforcement.
class TaskManager {
public:
    TaskManager() = default;
    ~TaskManager();

    TaskManager(const TaskManager&) = delete;
    TaskManager& operator=(const TaskManager&) = delete;

    /// Open `db_path` (":memory:" for tests; created if absent), apply WAL +
    /// foreign_keys pragmas, run the tasks DDL. Idempotent (CREATE IF NOT EXISTS).
    Status Init(const std::string& db_path = ":memory:");

    // ----- CRUD (S1) -----

    /// Insert a new task row. `task.task_id` is generated (ULID) when empty;
    /// created_at / updated_at are stamped now. Returns the stored TaskInfo.
    Result<TaskInfo> CreateTask(TaskInfo task);

    /// Fetch one task by id. Returns CX_ERR_TASK_NOT_FOUND (NotFound) if absent.
    Result<TaskInfo> GetTask(const std::string& task_id);

    /// Update the mutable progress columns of an in-flight task (driven by the
    /// F06 on_page_progress callback, S2): processed_pages / total_pages /
    /// failed_pages / progress_pct / eta_seconds / current_phase / worker_id.
    /// Stamps updated_at. NotFound if the task is gone.
    Status UpdateProgress(const TaskInfo& task);

    /// List tasks in a namespace, newest first (by created_at desc), paginated.
    Result<std::vector<TaskInfo>> ListByNamespace(const std::string& namespace_id,
                                                  int limit, int offset);

    // ----- State transitions (S1; driven by S2 scheduler / S3 cancel) -----

    /// queued → processing: stamp worker_id + started_at + updated_at (topic 2.3 /
    /// §4.2 Dequeue). NotFound if absent.
    Status MarkProcessing(const std::string& task_id, int worker_id);

    /// → completed: set doc_id, status=completed, progress_pct=100, completed_at.
    Status MarkCompleted(const std::string& task_id, const std::string& doc_id);

    /// → failed: set status=failed + error_code + error_msg + structured_data
    /// (§6.2 GEN-Agent persistence) + completed_at.
    Status MarkFailed(const std::string& task_id, const std::string& error_code,
                      const std::string& error_msg,
                      const std::string& structured_data = "");

    /// Mark the cancel intent (topic 3): set cancel_requested=1 and move status
    /// queued → cancelled (terminal) or processing → cancelling. A repeat on an
    /// already-cancelling task returns CX_ERR_TASK_CANCELLING (AlreadyExists) so
    /// the handler can emit a 423; an already-terminal task returns the same.
    /// On success `out` (if non-null) gets the post-transition row.
    Status RequestCancel(const std::string& task_id, TaskInfo* out);

    /// processing/cancelling → cancelled (terminal): the Worker reached its
    /// checkpoint and stopped. Stamps completed_at.
    Status MarkCancelled(const std::string& task_id);

    // ----- Scheduler-support queries (topic 2; consumed by TaskScheduler in S2) -----

    /// topic 2.2 — most recent task for `namespace_id` + `doc_id` + `task_type`
    /// whose created_at is within the last `window_seconds`, for the Watcher
    /// debounce check. nullopt if none. Scoping by task_type keeps different async
    /// kinds for the same doc_id (doc-parse vs F41 doc-summary) from debouncing
    /// each other.
    ///
    /// `namespace_id` is part of the identity, not a filter added for convenience:
    /// doc_id is chosen by the caller, so without it two namespaces submitting the
    /// same id debounce against each other — one namespace's document is silently
    /// merged into the other's task, and a refresh repoints that task at the other
    /// namespace's content while its namespace_id stays put.
    Result<std::optional<TaskInfo>> FindRecentTaskByDocId(const std::string& namespace_id,
                                                          const std::string& doc_id,
                                                          int task_type,
                                                          int window_seconds);

    /// topic 2.2 — a debounced re-submit with a *different* content_hash: refresh
    /// content_hash + filepath and reset progress to a fresh queued state.
    Result<TaskInfo> UpdateTaskForDebounce(const std::string& task_id,
                                           const SubmitRequest& req);

    /// topic 2.1 / 2.3 — the oldest status=queued task whose (namespace_id, doc_id)
    /// is NOT in `active_docs` (the per-doc mutex). nullopt if the queue is empty or
    /// all queued docs are already active. The pair is the unit of exclusion for the
    /// same reason it is the unit of debounce identity: a doc_id is only unique
    /// within its namespace, so excluding on the bare id makes one namespace's
    /// in-flight document block another's.
    Result<std::optional<TaskInfo>> SelectOldestQueuedTaskExcluding(
        const std::vector<std::pair<std::string, std::string>>& active_docs);

    // ----- Cleanup (topic 4; driven by the cron in task_cleanup_cron.*) -----

    /// topic 4.1 — delete completed/failed/cancelled tasks whose updated_at is
    /// older than `now_unix - retention_days*86400`. Returns the row count deleted.
    Result<int> DeleteExpired(int64_t now_unix, int retention_days);

    /// topic 4.3 — tasks still status=processing whose updated_at is older than
    /// `now_unix - zombie_hours*3600` are presumed crashed → set status=failed +
    /// error_code=CX_ERR_ZOMBIE_TASK_CLEANUP. Returns the row count swept.
    Result<int> SweepZombies(int64_t now_unix, int zombie_hours);

    /// §6.1 / §7 v0 timeout protection — tasks still status=processing whose started_at is
    /// older than `now_unix - timeout_seconds` have exceeded the processing budget
    /// → set status=failed + error_code=CX_ERR_TASK_TIMEOUT. Run AFTER SweepZombies
    /// so a 24h-stale (no-progress) row is recorded as a zombie rather than a
    /// timeout. Returns the row count timed out. (Driven by TaskCleanupCron, reading
    /// f42.task_timeout_seconds; sub-daily prompt enforcement → D3.5 server runtime.)
    Result<int> SweepTimedOut(int64_t now_unix, int timeout_seconds);

    /// Boundary — tasks left status=processing on a crash but younger than the zombie
    /// threshold are re-queued on restart (§6.1 / §7 crash recovery): status → queued,
    /// worker_id cleared. Returns the row count re-queued.
    Result<int> RequeueStaleProcessing(int64_t now_unix, int zombie_hours);

    /// (task_id, filepath) for every task that has NOT reached a terminal state
    /// and names an input. This is the live set a reaper must preserve: a file
    /// outside it has no task that will ever read it again.
    ///
    /// Deliberately returns raw rows rather than answering "is this path still
    /// referenced?" in SQL. The stored strings are whatever was handed to F42, so
    /// two rows can name the same file differently ("d/x.txt" vs "d/./x.txt"); a
    /// string comparison would miss the match and let a live input be deleted.
    /// Callers resolve both sides to one filesystem identity before comparing —
    /// the same identity used to remove the file.
    ///
    /// Fails closed: an interrupted scan is an error, never a shorter list.
    Result<std::vector<std::pair<std::string, std::string>>> LiveTaskInputs();

    /// Total row count (test aid).
    Result<int> CountAll();

  private:
    sqlite3* db_ = nullptr;
    bool owns_db_ = false;
    mutable std::mutex mutex_;

    Status CreateTasksTable();
};

}  // namespace cortrix::async
