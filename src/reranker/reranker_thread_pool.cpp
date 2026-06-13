#include "cortrix/reranker/reranker_thread_pool.h"

#include <algorithm>
#include <chrono>

namespace cortrix::reranker {

RerankerThreadPool::RerankerThreadPool(int num_workers, int queue_size, int task_timeout_ms)
    : queue_capacity_(static_cast<std::size_t>(std::max(1, queue_size))),
      task_timeout_ms_(task_timeout_ms) {
    int n = std::max(1, num_workers);
    workers_.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        workers_.emplace_back([this] { WorkerLoop(); });
    }
}

RerankerThreadPool::~RerankerThreadPool() { Shutdown(); }

std::future<float> RerankerThreadPool::Submit(std::function<float()> task) {
    std::packaged_task<float()> ptask(std::move(task));
    std::future<float> fut = ptask.get_future();
    {
        std::unique_lock<std::mutex> lock(mu_);
        // Backpressure: block until a slot frees (FIFO) or we are shutting down.
        not_full_.wait(lock, [this] { return queue_.size() < queue_capacity_ || stopping_; });
        if (stopping_) {
            // Pool is shutting down; fulfill the future with a broken-promise-free
            // value so the caller's wait does not hang.
            ptask.reset();
            std::promise<float> p;
            p.set_value(0.0f);
            return p.get_future();
        }
        queue_.push(std::move(ptask));
    }
    not_empty_.notify_one();
    return fut;
}

std::pair<float, bool> RerankerThreadPool::SubmitWaitFor(std::function<float()> task) {
    std::future<float> fut = Submit(std::move(task));
    if (fut.wait_for(std::chrono::milliseconds(task_timeout_ms_)) ==
        std::future_status::ready) {
        return {fut.get(), true};
    }
    // Timed out: abandon the future (the worker keeps running the task but its
    // result is discarded). Caller maps this to score=0 + timeout metric (S3.4).
    return {0.0f, false};
}

int RerankerThreadPool::QueueDepth() const {
    std::lock_guard<std::mutex> lock(mu_);
    return static_cast<int>(queue_.size());
}

void RerankerThreadPool::Shutdown() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (stopping_) return;
        stopping_ = true;
    }
    not_empty_.notify_all();
    not_full_.notify_all();
    for (auto& w : workers_) {
        if (w.joinable()) w.join();
    }
    workers_.clear();
}

void RerankerThreadPool::WorkerLoop() {
    for (;;) {
        std::packaged_task<float()> task;
        {
            std::unique_lock<std::mutex> lock(mu_);
            not_empty_.wait(lock, [this] { return !queue_.empty() || stopping_; });
            if (stopping_ && queue_.empty()) return;
            task = std::move(queue_.front());
            queue_.pop();
        }
        // A queue slot just freed → wake one blocked Submit (FIFO order preserved
        // because Submit re-checks capacity under the lock).
        not_full_.notify_one();
        task();  // runs the packaged_task, fulfilling its future
    }
}

}  // namespace cortrix::reranker
