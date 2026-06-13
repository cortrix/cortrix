#include "cortrix/common/executor_engine.h"

#include <utility>

namespace cortrix {

ExecutorEngine::ExecutorEngine(int workers, int queue_size)
    : queue_capacity_(queue_size < 1 ? 1u : static_cast<std::size_t>(queue_size)) {
    const int n = workers < 1 ? 1 : workers;
    workers_.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        workers_.emplace_back(&ExecutorEngine::WorkerLoop, this);
    }
}

ExecutorEngine::~ExecutorEngine() {
    Shutdown();
}

void ExecutorEngine::Enqueue(std::function<void()> job) {
    {
        std::unique_lock<std::mutex> lk(mu_);
        not_full_.wait(lk, [this] { return queue_.size() < queue_capacity_ || shutdown_; });
        if (shutdown_) return;  // dropped; the packaged_task future becomes broken
        queue_.push(std::move(job));
    }
    not_empty_.notify_one();
}

void ExecutorEngine::WorkerLoop() {
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock<std::mutex> lk(mu_);
            not_empty_.wait(lk, [this] { return !queue_.empty() || shutdown_; });
            if (queue_.empty()) {
                if (shutdown_) return;  // queue drained and stopping
                continue;
            }
            job = std::move(queue_.front());
            queue_.pop();
            ++active_workers_;
        }
        not_full_.notify_one();  // a queue slot freed up
        job();
        {
            std::lock_guard<std::mutex> lk(mu_);
            --active_workers_;
        }
    }
}

int ExecutorEngine::QueueDepth() const {
    std::lock_guard<std::mutex> lk(mu_);
    return static_cast<int>(queue_.size());
}

int ExecutorEngine::ActiveWorkers() const {
    std::lock_guard<std::mutex> lk(mu_);
    return active_workers_;
}

void ExecutorEngine::Shutdown() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (shutdown_) return;
        shutdown_ = true;
    }
    not_empty_.notify_all();
    not_full_.notify_all();
    for (auto& w : workers_) {
        if (w.joinable()) w.join();
    }
}

}  // namespace cortrix
