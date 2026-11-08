#include "cortrix/spc/spc_task_queue.h"
#include <algorithm>

namespace cortrix {

SPCTaskQueue::SPCTaskQueue(int max_size)
    : max_size_(max_size) {}

int SPCTaskQueue::Push(std::shared_ptr<SPCTask> task) {
    std::lock_guard<std::mutex> lock(mu_);
    if (stopped_) return -1;
    if (static_cast<int>(queue_.size()) >= max_size_) return -1;

    queue_.push_back(std::move(task));

    // Sort by priority (lower value = higher priority), then by created_at (FIFO)
    std::sort(queue_.begin(), queue_.end(),
        [](const std::shared_ptr<SPCTask>& a, const std::shared_ptr<SPCTask>& b) {
            int pa = static_cast<int>(a->priority);
            int pb = static_cast<int>(b->priority);
            if (pa != pb) return pa < pb;
            return a->created_at_us < b->created_at_us;
        });

    cv_.notify_one();
    return 0;
}

std::shared_ptr<SPCTask> SPCTaskQueue::Pop() {
    std::unique_lock<std::mutex> lock(mu_);
    for (;;) {
        // Drop cancelled tasks.
        queue_.erase(std::remove_if(queue_.begin(), queue_.end(),
                                    [](const std::shared_ptr<SPCTask>& t) {
                                        return !t || t->cancelled.load();
                                    }),
                     queue_.end());

        // Take the first task (in priority order) whose document has nothing running.
        // Two tasks for one document must not run at once: the second one's write is
        // refused while the first holds the document's write transaction, and its work
        // is lost (several interactions written to one memory session in a row).
        // A task without a document id has nothing to wait for.
        auto it = std::find_if(queue_.begin(), queue_.end(),
                               [this](const std::shared_ptr<SPCTask>& t) {
                                   return t->doc_id.empty() ||
                                          running_.find(t->doc_id) == running_.end();
                               });
        if (it != queue_.end()) {
            auto task = *it;
            queue_.erase(it);
            // Running from the moment it leaves the queue, under the same lock, so the
            // task is never seen as neither queued nor running.
            ++running_[task->doc_id];
            return task;
        }
        if (stopped_) return nullptr;
        cv_.wait(lock);  // Push, FinishTask and Stop notify
    }
}

void SPCTaskQueue::FinishTask(const std::string& doc_id) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = running_.find(doc_id);
    if (it == running_.end()) return;
    if (--it->second <= 0) {
        running_.erase(it);
        cv_.notify_all();  // a queued task for this document may be runnable now
    }
}

void SPCTaskQueue::BeginRunning(const std::string& doc_id) {
    std::lock_guard<std::mutex> lock(mu_);
    ++running_[doc_id];
}

bool SPCTaskQueue::IsRunning(const std::string& doc_id) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = running_.find(doc_id);
    return it != running_.end() && it->second > 0;
}

int SPCTaskQueue::CancelQueuedVersions(const std::string& namespace_name,
                                        const std::string& source_type,
                                        const std::string& source_path) {
    std::lock_guard<std::mutex> lock(mu_);
    int count = 0;
    for (auto& task : queue_) {
        // All three: one queue serves every namespace, and a path alone also matches
        // a same-named file of another namespace or of another producer.
        if (task->namespace_name == namespace_name && task->source_type == source_type &&
            task->source_path == source_path && !task->cancelled.load()) {
            task->cancelled.store(true);
            task->stage = SPCStage::kCancelled;
            ++count;
        }
    }
    return count;
}

bool SPCTaskQueue::HasQueuedTask(const std::string& doc_id) const {
    std::lock_guard<std::mutex> lock(mu_);
    for (const auto& task : queue_) {
        if (task && task->doc_id == doc_id && !task->cancelled.load()) return true;
    }
    return false;
}

void SPCTaskQueue::Stop() {
    std::lock_guard<std::mutex> lock(mu_);
    stopped_ = true;
    cv_.notify_all();
}

std::vector<std::shared_ptr<SPCTask>> SPCTaskQueue::DrainRemaining() {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<std::shared_ptr<SPCTask>> remaining;
    remaining.reserve(queue_.size());
    for (auto& task : queue_) {
        if (task && !task->cancelled.load()) {
            remaining.push_back(std::move(task));
        }
    }
    queue_.clear();
    return remaining;
}

size_t SPCTaskQueue::Size() const {
    std::lock_guard<std::mutex> lock(mu_);
    return queue_.size();
}

bool SPCTaskQueue::IsStopped() const {
    std::lock_guard<std::mutex> lock(mu_);
    return stopped_;
}

}  // namespace cortrix
