#include <cstdint>
#include "cortrix/async/task_scheduler.h"

#include "cortrix/async/f42_error.h"
#include "cortrix/async/f42_metrics.h"

namespace cortrix::async {

TaskScheduler::TaskScheduler(TaskManager* mgr, const IGlobalConfig* config)
    : mgr_(mgr), config_(config) {}

int TaskScheduler::DebounceSeconds() const {
    if (config_) {
        auto r = config_->GetInt("f42.watcher_debounce_seconds");
        if (r.ok()) return r.value();
    }
    return kDefaultDebounceSeconds;
}

Result<TaskInfo> TaskScheduler::Enqueue(const SubmitRequest& req) {
    std::lock_guard<std::mutex> lock(mutex_);

    // topic 2.2 C — 5s Watcher debounce. Only meaningful when we have a doc_id to
    // dedup on (Phase 1 doc_id = content-hash-derived identity) and a positive
    // window (window <= 0 disables debounce → every submit is a fresh task).
    const int window = DebounceSeconds();
    if (!req.doc_id.empty() && window > 0) {
        // Identity is (namespace_id, doc_id, task_type). doc_id alone is chosen by
        // the caller and therefore only unique within a namespace.
        auto recent = mgr_->FindRecentTaskByDocId(req.namespace_id, req.doc_id,
                                                  req.task_type, window);
        if (!recent.ok()) return recent.status();
        if (recent.value().has_value()) {
            const TaskInfo& r = *recent.value();
            // `r` is a snapshot: RequestCancel does not share this mutex, so the
            // candidate can go terminal between the lookup and the decision below.
            // Neither branch may act on that snapshot — each has an authoritative
            // operation that re-checks under the task manager's own lock, and
            // losing that ordering means falling through to a task of our own.
            if (r.content_hash == req.content_hash) {
                // same doc + same content_hash within window → merge (no new row).
                // Merging on a stale snapshot would swallow the resubmission: the
                // caller gets a task that will never carry the content, and the
                // input materialized for it is released.
                auto claimed = mgr_->TryClaimDebounceMerge(r.task_id);
                if (claimed.ok()) {
                    // The merge won the ordering; a cancel arriving now applies to
                    // the merged task, which is coherent. That task keeps its own
                    // filepath, so the input materialized for THIS submission is
                    // owned by nobody — hand it back rather than leak it.
                    if (!req.filepath.empty() &&
                        req.filepath != claimed.value().filepath) {
                        ReleaseUnadoptedInput(req.filepath, claimed.value().task_id);
                    }
                    return claimed;
                }
            } else if (r.status == task_status::kQueued) {
                // same doc + different content_hash within window → refresh + reset.
                // The conditional UPDATE is the authority; the snapshot check only
                // skips a write we already know is pointless for an in-flight row.
                // A reset-to-queued is a fresh submission for metrics (§6.bis).
                const std::string superseded = r.filepath;
                auto refreshed = mgr_->UpdateTaskForDebounce(r.task_id, req);
                if (refreshed.ok()) {
                    F42Metrics::Instance().RecordSubmitted(
                        static_cast<TaskType>(req.task_type));
                    // The row now points at the new input; the previous one is
                    // orphaned the moment that write lands.
                    if (!superseded.empty() && superseded != req.filepath) {
                        ReleaseUnadoptedInput(superseded, r.task_id);
                    }
                    return refreshed;
                }
            }
        }
    }

    // topic 2.1 A — new queued task. Same doc_id already processing is allowed;
    // the per-doc_id mutex (Dequeue/active_doc_ids_) serializes it FIFO.
    TaskInfo task;
    task.namespace_id = req.namespace_id;
    task.filename = req.filename;
    task.filepath = req.filepath;
    task.doc_id = req.doc_id;
    task.content_hash = req.content_hash;
    task.trace_id = req.trace_id;  // topic 6 — populated during D3 implementation
    task.metadata_json = req.metadata_json;  // caller doc metadata → persisted on the task
    task.total_pages = req.total_pages;
    task.task_type = req.task_type;
    task.status = task_status::kQueued;
    auto created = mgr_->CreateTask(std::move(task));
    if (created.ok()) {
        F42Metrics::Instance().RecordSubmitted(static_cast<TaskType>(req.task_type));
    }
    return created;
}

Result<std::optional<TaskInfo>> TaskScheduler::Dequeue(int worker_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    // topic 2.3 B — exclude (namespace, doc) pairs already being processed.
    std::vector<std::pair<std::string, std::string>> active(active_docs_.begin(),
                                                            active_docs_.end());
    auto picked = mgr_->SelectOldestQueuedTaskExcluding(active);
    if (!picked.ok()) return picked.status();
    if (!picked.value().has_value()) return std::optional<TaskInfo>{};

    TaskInfo task = *picked.value();
    // Reserve the doc_id before marking processing, so a concurrent Dequeue on
    // another worker can't also pick a task for the same doc_id.
    if (!task.doc_id.empty()) active_docs_.insert({task.namespace_id, task.doc_id});

    Status s = mgr_->MarkProcessing(task.task_id, worker_id);
    if (!s.ok()) {
        if (!task.doc_id.empty()) active_docs_.erase({task.namespace_id, task.doc_id});
        // The row stopped being queued between the select and the mark — cancelled
        // (which also released its input) or claimed by another worker. Report "no
        // task available" rather than an error: dispatching it would hand a worker a
        // task whose input may already be gone, and the caller simply tries again.
        const char* conflict = F42ErrorCodeString(F42ErrorCode::kDocProcessingInProgress);
        if (s.message().rfind(conflict, 0) == 0) {
            return std::optional<TaskInfo>{};
        }
        return s;
    }
    task.status = task_status::kProcessing;
    task.worker_id = worker_id;
    // §6.bis cortrix_tasks_queue_depth{state="processing"}: the in-flight doc set
    // is this scheduler's authoritative processing count within the process.
    F42Metrics::Instance().SetQueueDepth(
        F42Metrics::QueueState::kProcessing,
        static_cast<int64_t>(active_docs_.size()));
    return std::optional<TaskInfo>{task};
}

void TaskScheduler::OnTaskCompleted(const std::string& namespace_id,
                                    const std::string& doc_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!doc_id.empty()) active_docs_.erase({namespace_id, doc_id});
    F42Metrics::Instance().SetQueueDepth(
        F42Metrics::QueueState::kProcessing,
        static_cast<int64_t>(active_docs_.size()));
}

size_t TaskScheduler::ActiveDocCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_docs_.size();
}

bool TaskScheduler::IsDocActive(const std::string& namespace_id,
                                const std::string& doc_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_docs_.count({namespace_id, doc_id}) > 0;
}

void TaskScheduler::ReleaseUnadoptedInput(const std::string& filepath,
                                          const std::string& owner_task_id) const {
    // Wired at bootstrap to the batch temp store. Unset (standalone/test) means the
    // caller owns its own cleanup; never guess a directory here.
    if (unadopted_input_releaser_) unadopted_input_releaser_(filepath, owner_task_id);
}

}  // namespace cortrix::async
