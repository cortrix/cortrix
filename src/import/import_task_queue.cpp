#include "cortrix/import/import_task_queue.h"

#include <chrono>

#include "cortrix/import/import_error.h"

namespace cortrix::import {

namespace {
std::chrono::system_clock::time_point Now() { return std::chrono::system_clock::now(); }
}  // namespace

// --- InMemoryImportTaskStore ---

Status InMemoryImportTaskStore::Create(const ImportTaskProgress& task) {
    std::lock_guard<std::mutex> lock(mu_);
    if (tasks_.count(task.task_id)) {
        return Status::AlreadyExists("import task already exists: " + task.task_id);
    }
    tasks_[task.task_id] = task;
    return Status::Ok();
}

std::optional<ImportTaskProgress> InMemoryImportTaskStore::Get(const ImportTaskId& task_id) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = tasks_.find(task_id);
    if (it == tasks_.end()) return std::nullopt;
    return it->second;
}

Status InMemoryImportTaskStore::Update(const ImportTaskProgress& task) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = tasks_.find(task.task_id);
    if (it == tasks_.end()) return Status::NotFound("import task not found: " + task.task_id);
    it->second = task;
    return Status::Ok();
}

bool InMemoryImportTaskStore::CompareAndSetStatus(const ImportTaskId& task_id,
                                                  ImportTaskStatus expected,
                                                  ImportTaskStatus next) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = tasks_.find(task_id);
    if (it == tasks_.end() || it->second.status != expected) return false;
    it->second.status = next;
    return true;
}

std::vector<ImportTaskProgress> InMemoryImportTaskStore::ListByNamespace(const NsId& ns) {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<ImportTaskProgress> out;
    for (const auto& [id, task] : tasks_) {
        if (task.namespace_id == ns) out.push_back(task);
    }
    return out;
}

// --- ImportTaskHandle ---

void ImportTaskHandle::ReportProgress(int rows_imported, int rows_total) {
    auto cur = store_->Get(id_);
    if (!cur) return;
    cur->rows_imported = rows_imported;
    if (rows_total > 0) cur->rows_total = rows_total;
    cur->progress =
        (cur->rows_total > 0)
            ? static_cast<double>(rows_imported) / static_cast<double>(cur->rows_total)
            : 0.0;
    store_->Update(*cur);
}

// --- ImportTaskQueue ---

ImportTaskQueue::ImportTaskQueue(std::shared_ptr<IImportTaskStore> store,
                                 ImportTaskQueueConfig config)
    : store_(std::move(store)),
      config_(config),
      engine_(std::make_unique<cortrix::ExecutorEngine>(config.worker_count,
                                                        config.queue_max_size)) {}

ImportTaskQueue::~ImportTaskQueue() {
    // Stop accepting + drain dispatched work before tearing down (workers reference
    // store_ / cancel flags). ExecutorEngine::Shutdown joins worker threads.
    DrainForTest();
    if (engine_) engine_->Shutdown();
}

Result<ImportTaskId> ImportTaskQueue::Submit(const ImportTaskId& task_id,
                                             const NsId& namespace_id,
                                             ImportTaskWork work,
                                             int rows_total) {
    {
        std::lock_guard<std::mutex> lock(cancel_mu_);
        if (static_cast<int>(cancel_flags_.size()) >= config_.queue_max_size) {
            // Queue full — surface as a transient (the Agent can retry after backoff).
            return ImportStatus(ImportErrorCode::kConnectionFailed,
                              "import task queue is full (max " +
                                  std::to_string(config_.queue_max_size) + ")");
        }
        cancel_flags_[task_id] = std::make_shared<std::atomic<bool>>(false);
    }

    ImportTaskProgress task;
    task.task_id = task_id;
    task.status = ImportTaskStatus::kQueued;
    task.namespace_id = namespace_id;
    task.rows_total = rows_total;
    task.queued_at = Now();
    Status created = store_->Create(task);
    if (!created.ok()) {
        std::lock_guard<std::mutex> lock(cancel_mu_);
        cancel_flags_.erase(task_id);
        return created;
    }

    // Dispatch to a worker thread (ExecutorEngine — shared pool, NOT the async scheduler). The
    // per-task work closure (with its captured auth/trace ctx) rides along.
    std::future<void> fut = engine_->Submit<void>(
        [this, task_id, work = std::move(work)]() mutable { RunTask(task_id, std::move(work)); });
    {
        std::lock_guard<std::mutex> lock(cancel_mu_);
        inflight_.push_back(std::move(fut));
    }
    return task_id;
}

void ImportTaskQueue::RunTask(ImportTaskId task_id, ImportTaskWork work) {
    std::shared_ptr<std::atomic<bool>> cancel_flag;
    {
        std::lock_guard<std::mutex> lock(cancel_mu_);
        auto it = cancel_flags_.find(task_id);
        if (it != cancel_flags_.end()) cancel_flag = it->second;
    }
    if (!cancel_flag) return;  // submit was rolled back

    // QUEUED → RUNNING. If a cancel already flipped us to CANCELLING/CANCELLED while
    // queued, the CAS fails and we finalize as CANCELLED without running the work.
    if (!store_->CompareAndSetStatus(task_id, ImportTaskStatus::kQueued,
                                     ImportTaskStatus::kRunning)) {
        if (auto t = store_->Get(task_id);
            t && (t->status == ImportTaskStatus::kCancelling ||
                  t->status == ImportTaskStatus::kCancelled)) {
            store_->CompareAndSetStatus(task_id, ImportTaskStatus::kCancelling,
                                        ImportTaskStatus::kCancelled);
        }
        return;
    }
    if (auto t = store_->Get(task_id)) {
        t->started_at = Now();
        store_->Update(*t);
    }

    ImportTaskHandle handle(task_id, store_.get(), cancel_flag);
    Status work_status = work(handle);

    // Resolve the terminal state. A cancel (flag set / CANCELLING) wins over a
    // success: the worker stopped cooperatively, so the task is CANCELLED not FAILED.
    auto finalize = [&](ImportTaskStatus terminal, const Status* err) {
        auto t = store_->Get(task_id);
        if (!t) return;
        t->status = terminal;
        t->completed_at = Now();
        if (terminal == ImportTaskStatus::kCompleted) {
            t->progress = 1.0;
            if (t->rows_total <= 0) t->rows_total = t->rows_imported;
        }
        if (terminal == ImportTaskStatus::kFailed && err) {
            // Re-inflate the CX_ERR_IMPORT_* identity carried in the Status message
            // into the Agent-friendly error body (§5.3).
            t->error = MakeImportError(ImportErrorCode::kConnectionFailed, nlohmann::json::object(),
                                     err->message());
            // Prefer the precise code if the work returned one in the message prefix.
            // (The work uses ImportStatus(...) which prefixes "CX_ERR_IMPORT_X: detail".)
            t->error->code = [&]() -> std::string {
                const std::string& m = err->message();
                auto colon = m.find(':');
                if (m.rfind("CX_ERR_IMPORT_", 0) == 0 && colon != std::string::npos) {
                    return m.substr(0, colon);
                }
                return t->error->code;
            }();
        }
        store_->Update(*t);
    };

    if (cancel_flag->load()) {
        finalize(ImportTaskStatus::kCancelled, nullptr);
    } else if (work_status.ok()) {
        finalize(ImportTaskStatus::kCompleted, nullptr);
    } else {
        finalize(ImportTaskStatus::kFailed, &work_status);
    }

    // Drop the cancel flag (task is terminal).
    {
        std::lock_guard<std::mutex> lock(cancel_mu_);
        cancel_flags_.erase(task_id);
    }
}

std::optional<ImportTaskProgress> ImportTaskQueue::GetProgress(const ImportTaskId& task_id) {
    return store_->Get(task_id);
}

bool ImportTaskQueue::Cancel(const ImportTaskId& task_id) {
    std::shared_ptr<std::atomic<bool>> flag;
    {
        std::lock_guard<std::mutex> lock(cancel_mu_);
        auto it = cancel_flags_.find(task_id);
        if (it != cancel_flags_.end()) flag = it->second;
    }
    auto cur = store_->Get(task_id);
    if (!cur) return false;  // unknown task

    if (flag) flag->store(true);  // signal the cooperative-cancel poll point

    // QUEUED → CANCELLED directly (never ran). RUNNING → CANCELLING (worker stops at
    // its next checkpoint, then RunTask finalizes CANCELLED). Terminal = no-op.
    if (store_->CompareAndSetStatus(task_id, ImportTaskStatus::kQueued,
                                    ImportTaskStatus::kCancelled)) {
        return true;
    }
    store_->CompareAndSetStatus(task_id, ImportTaskStatus::kRunning,
                                ImportTaskStatus::kCancelling);
    return true;
}

void ImportTaskQueue::DrainForTest() {
    std::vector<std::future<void>> futures;
    {
        std::lock_guard<std::mutex> lock(cancel_mu_);
        futures.swap(inflight_);
    }
    for (auto& f : futures) {
        if (f.valid()) f.wait();
    }
}

}  // namespace cortrix::import
