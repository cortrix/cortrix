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
    cv_.wait(lock, [this] { return stopped_ || !queue_.empty(); });

    if (stopped_ && queue_.empty()) return nullptr;
    if (queue_.empty()) return nullptr;

    // Take highest priority (front of sorted queue)
    auto task = queue_.front();
    queue_.erase(queue_.begin());

    // Skip cancelled tasks
    while (task && task->cancelled.load() && !queue_.empty()) {
        task = queue_.front();
        queue_.erase(queue_.begin());
    }
    if (task && task->cancelled.load()) return nullptr;

    // Running from the moment it leaves the queue, under the same lock, so the task is
    // never seen as neither queued nor running.
    if (task) ++running_[task->doc_id];
    return task;
}

void SPCTaskQueue::FinishTask(const std::string& doc_id) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = running_.find(doc_id);
    if (it == running_.end()) return;
    if (--it->second <= 0) running_.erase(it);
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
