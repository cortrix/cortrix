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
        auto recent = mgr_->FindRecentTaskByDocId(req.doc_id, req.task_type, window);
        if (!recent.ok()) return recent.status();
        if (recent.value().has_value()) {
            const TaskInfo& r = *recent.value();
            if (r.content_hash == req.content_hash) {
                // same doc_id + same content_hash within window → merge (no new row).
                return r;
            }
            // same doc_id + different content_hash within window → refresh + reset.
            // A reset-to-queued is a fresh submission for metric purposes (§6.bis).
            auto refreshed = mgr_->UpdateTaskForDebounce(r.task_id, req);
            if (refreshed.ok()) {
                F42Metrics::Instance().RecordSubmitted(
                    static_cast<TaskType>(req.task_type));
            }
            return refreshed;
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

    // topic 2.3 B — exclude doc_ids already being processed (in-memory set).
    std::vector<std::string> active(active_doc_ids_.begin(), active_doc_ids_.end());
    auto picked = mgr_->SelectOldestQueuedTaskExcluding(active);
    if (!picked.ok()) return picked.status();
    if (!picked.value().has_value()) return std::optional<TaskInfo>{};

    TaskInfo task = *picked.value();
    // Reserve the doc_id before marking processing, so a concurrent Dequeue on
    // another worker can't also pick a task for the same doc_id.
    if (!task.doc_id.empty()) active_doc_ids_.insert(task.doc_id);

    Status s = mgr_->MarkProcessing(task.task_id, worker_id);
    if (!s.ok()) {
        if (!task.doc_id.empty()) active_doc_ids_.erase(task.doc_id);
        return s;
    }
    task.status = task_status::kProcessing;
    task.worker_id = worker_id;
    // §6.bis cortrix_tasks_queue_depth{state="processing"}: the in-flight doc set
    // is this scheduler's authoritative processing count within the process.
    F42Metrics::Instance().SetQueueDepth(
        F42Metrics::QueueState::kProcessing,
        static_cast<int64_t>(active_doc_ids_.size()));
    return std::optional<TaskInfo>{task};
}

void TaskScheduler::OnTaskCompleted(const std::string& doc_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!doc_id.empty()) active_doc_ids_.erase(doc_id);
    F42Metrics::Instance().SetQueueDepth(
        F42Metrics::QueueState::kProcessing,
        static_cast<int64_t>(active_doc_ids_.size()));
}

size_t TaskScheduler::ActiveDocCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_doc_ids_.size();
}

bool TaskScheduler::IsDocActive(const std::string& doc_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_doc_ids_.count(doc_id) > 0;
}

}  // namespace cortrix::async
