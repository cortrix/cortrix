#pragma once
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "cortrix/common/executor_engine.h"
#include "cortrix/common/result.h"
#include "cortrix/import/import_types.h"

namespace cortrix::import {

/// Persistence seam for the D6 task lifecycle (F16a §4.2 import_tasks). cortrix/ owns
/// this CE interface; the SQLite-backed store (real import_tasks rows) is D3.5.
/// Standalone uses InMemoryImportTaskStore. Mutating ops are CAS-on-status so a
/// cancel racing a worker resolves deterministically.
class IImportTaskStore {
public:
    virtual ~IImportTaskStore() = default;

    virtual Status Create(const ImportTaskProgress& task) = 0;
    virtual std::optional<ImportTaskProgress> Get(const ImportTaskId& task_id) = 0;
    /// Persist a progress update (status / counters / timestamps / error). Last write
    /// wins on the mutable fields; identity fields are immutable after Create.
    virtual Status Update(const ImportTaskProgress& task) = 0;
    /// Atomically set status iff the current status == `expected`. Returns false (no
    /// write) if the precondition fails — the basis of the cancel/worker race rules.
    virtual bool CompareAndSetStatus(const ImportTaskId& task_id,
                                     ImportTaskStatus expected,
                                     ImportTaskStatus next) = 0;
    virtual std::vector<ImportTaskProgress> ListByNamespace(const NsId& ns) = 0;
};

/// In-memory IImportTaskStore — standalone default + unit test double. Thread-safe.
class InMemoryImportTaskStore : public IImportTaskStore {
public:
    Status Create(const ImportTaskProgress& task) override;
    std::optional<ImportTaskProgress> Get(const ImportTaskId& task_id) override;
    Status Update(const ImportTaskProgress& task) override;
    bool CompareAndSetStatus(const ImportTaskId& task_id, ImportTaskStatus expected,
                             ImportTaskStatus next) override;
    std::vector<ImportTaskProgress> ListByNamespace(const NsId& ns) override;

private:
    std::mutex mu_;
    std::map<ImportTaskId, ImportTaskProgress> tasks_;
};

/// Handle a running worker uses to report progress + check for a cancel request
/// (F16a §3.6 / §5.2). The worker periodically calls CancelRequested(); on true it
/// must stop, leave partial rows, and let the queue mark CANCELLED.
class ImportTaskHandle {
public:
    ImportTaskHandle(ImportTaskId id, IImportTaskStore* store,
                     std::shared_ptr<std::atomic<bool>> cancel_flag)
        : id_(std::move(id)), store_(store), cancel_flag_(std::move(cancel_flag)) {}

    const ImportTaskId& id() const { return id_; }

    /// True once cancel() was called for this task — the worker's cooperative-cancel
    /// poll point (§3.6 CANCELLING → the worker stops at the next checkpoint).
    bool CancelRequested() const { return cancel_flag_->load(); }

    /// Report incremental progress (rows_imported / rows_total → progress ratio).
    void ReportProgress(int rows_imported, int rows_total);

private:
    ImportTaskId id_;
    IImportTaskStore* store_;
    std::shared_ptr<std::atomic<bool>> cancel_flag_;
};

/// The unit of work a submitted task runs. The ImportManager supplies
/// this PER Submit (so each task carries its own captured auth / trace context):
/// fetch → textualize → cleanup → feed-to-SPC, reporting progress + honoring
/// handle.CancelRequested(). Returns Ok on success; an error Status (a CX_ERR_F16A_*
/// Status, e.g. from F16aStatus) drives the task to FAILED with that code/message.
using ImportTaskWork = std::function<Status(ImportTaskHandle&)>;

struct ImportTaskQueueConfig {
    int worker_count = 2;        // F16a §4.4 task.worker_count (limit external PG pressure)
    int queue_max_size = 100;    // §4.4 task.queue_max_size
};

/// D6 async import queue. **Self-built in cortrix::import, modeled on the
/// F42 async pattern but with ZERO dependency on F42** (the §0 red line: no
/// `#include` / link of cortrix::async::TaskScheduler). Worker threads come from the
/// shared cortrix::ExecutorEngine (common, Wave-A frozen — NOT F42); the lifecycle
/// state machine + import_tasks persistence are owned here.
///
/// State machine (§3.6): QUEUED → RUNNING → {COMPLETED | FAILED | CANCELLED}, with
/// CANCELLING as the intermediate a cancel sets. A cancel before RUNNING resolves to
/// CANCELLED directly; during RUNNING it sets CANCELLING and the worker stops
/// cooperatively at its next CancelRequested() checkpoint.
class ImportTaskQueue {
public:
    ImportTaskQueue(std::shared_ptr<IImportTaskStore> store,
                    ImportTaskQueueConfig config = {});
    ~ImportTaskQueue();

    ImportTaskQueue(const ImportTaskQueue&) = delete;
    ImportTaskQueue& operator=(const ImportTaskQueue&) = delete;

    /// Enqueue an import. Persists a QUEUED row, returns its task_id. `work` is the
    /// per-task closure (captures the caller's auth / trace context). `rows_total`
    /// (if known from the COUNT(*) pre-estimate) seeds progress; 0 = unknown until
    /// the worker reports. The work is dispatched to a worker thread.
    Result<ImportTaskId> Submit(const ImportTaskId& task_id,
                                const NsId& namespace_id,
                                ImportTaskWork work,
                                int rows_total = 0);

    /// Current progress snapshot (§5.2). nullopt if the task_id is unknown.
    std::optional<ImportTaskProgress> GetProgress(const ImportTaskId& task_id);

    /// Request cancellation (§3.6). QUEUED → CANCELLED immediately; RUNNING →
    /// CANCELLING (the worker finishes the current row then stops). Terminal states
    /// are no-ops. Returns false only for an unknown task_id.
    bool Cancel(const ImportTaskId& task_id);

    /// Block until all dispatched work finishes (test/shutdown helper).
    void DrainForTest();

    int QueueDepth() const { return engine_->QueueDepth(); }

private:
    void RunTask(ImportTaskId task_id, ImportTaskWork work);

    std::shared_ptr<IImportTaskStore> store_;
    ImportTaskQueueConfig config_;
    std::unique_ptr<cortrix::ExecutorEngine> engine_;

    std::mutex cancel_mu_;
    std::map<ImportTaskId, std::shared_ptr<std::atomic<bool>>> cancel_flags_;  // guarded by cancel_mu_
    std::vector<std::future<void>> inflight_;                                   // guarded by cancel_mu_
};

}  // namespace cortrix::import
