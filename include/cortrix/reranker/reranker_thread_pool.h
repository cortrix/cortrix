#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <thread>
#include <utility>
#include <vector>

namespace cortrix::reranker {

/// RerankerThreadPool — F02's OWN bounded-queue thread pool (F02 §3.1 / topic 3.3).
///
/// D3 standalone (F02-3 → D3.5): F02 deliberately keeps its own pool this phase;
/// sharing the global cortrix::ExecutorEngine ("ThreadPool→ExecutorEngine sharing",
/// F02-3 hook) is cross-Feature integration deferred to D3.5. The interface here
/// mirrors the spec (`std::future<R> Submit(std::function<R()>)`) so the later swap
/// to ExecutorEngine keeps `reranker.workers` GUC semantics.
///
/// Result type R is a template parameter so a task can return its result BY VALUE
/// (no write-back through captured references). This is the lifetime contract that
/// makes a timed-out task safe (see below): R02-C2 — on a per-task timeout the
/// caller abandons the future, but the worker keeps running the abandoned task. If
/// the task wrote back into caller-stack state captured by reference, that later
/// write would land on a destroyed / reused stack slot (use-after-free) and race
/// the next loop iteration. By having the task return R by value, every result —
/// including an abandoned task's — lands inside that task's own future and is
/// simply discarded on timeout; the worker never touches the caller's frame.
/// OnnxReranker::ScoreBatch uses R = RerankTaskResult; LlmEnricher keeps R = float
/// (its payload travels via a captured shared_ptr<Slot>, already lifetime-safe).
///
/// Behavior (topic 1.2 / 1.3 / 3.3):
///   - N (default 4) resident worker threads drain a bounded FIFO queue.
///   - Submit() BLOCKS when the queue is full (backpressure; no dropped request).
///   - task_timeout_ms is the per-task budget enforced by the CALLER via
///     SubmitWaitFor()/std::future::wait_for (the pool stores it so callers don't
///     restate it). A timed-out task is abandoned to the worker (its future is
///     dropped); the caller treats it as score=0 (S3.4) + records the timeout.
template <typename R>
class RerankerThreadPool {
public:
    /// @param num_workers     resident worker count (clamped to >= 1)
    /// @param queue_size      max queued-but-not-running tasks (clamped to >= 1)
    /// @param task_timeout_ms per-task budget surfaced to callers (stored only)
    RerankerThreadPool(int num_workers, int queue_size, int task_timeout_ms)
        : queue_capacity_(static_cast<std::size_t>(std::max(1, queue_size))),
          task_timeout_ms_(task_timeout_ms) {
        int n = std::max(1, num_workers);
        workers_.reserve(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i) {
            workers_.emplace_back([this] { WorkerLoop(); });
        }
    }

    ~RerankerThreadPool() { Shutdown(); }

    RerankerThreadPool(const RerankerThreadPool&) = delete;
    RerankerThreadPool& operator=(const RerankerThreadPool&) = delete;

    /// Enqueue a scoring task; returns its future. BLOCKS while the queue is full
    /// (FIFO). Must not be called after Shutdown().
    std::future<R> Submit(std::function<R()> task) {
        std::packaged_task<R()> ptask(std::move(task));
        std::future<R> fut = ptask.get_future();
        {
            std::unique_lock<std::mutex> lock(mu_);
            // Backpressure: block until a slot frees (FIFO) or we are shutting down.
            not_full_.wait(lock, [this] { return queue_.size() < queue_capacity_ || stopping_; });
            if (stopping_) {
                // Pool is shutting down; fulfill the future with a broken-promise-free
                // value so the caller's wait does not hang.
                ptask.reset();
                std::promise<R> p;
                p.set_value(R{});
                return p.get_future();
            }
            queue_.push(std::move(ptask));
        }
        not_empty_.notify_one();
        return fut;
    }

    /// Submit + wait up to task_timeout_ms. Returns {value, true} on completion,
    /// or {R{}, false} on timeout (caller maps timeout → score=0 + metric, S3.4).
    /// On timeout the future is abandoned: the worker still runs the task to
    /// completion, but its by-value result lands inside the dropped future and is
    /// never written back to the caller (R02-C2 use-after-free / data-race fix).
    std::pair<R, bool> SubmitWaitFor(std::function<R()> task) {
        std::future<R> fut = Submit(std::move(task));
        if (fut.wait_for(std::chrono::milliseconds(task_timeout_ms_)) ==
            std::future_status::ready) {
            return {fut.get(), true};
        }
        // Timed out: abandon the future (the worker keeps running the task but its
        // result is discarded). Caller maps this to score=0 + timeout metric (S3.4).
        return {R{}, false};
    }

    int num_workers() const { return static_cast<int>(workers_.size()); }
    int task_timeout_ms() const { return task_timeout_ms_; }
    std::size_t queue_capacity() const { return queue_capacity_; }

    /// Current queue depth (tasks queued, not yet started) —
    /// cortrix_reranker_queue_depth_current gauge source.
    int QueueDepth() const {
        std::lock_guard<std::mutex> lock(mu_);
        return static_cast<int>(queue_.size());
    }

    /// Drain queued tasks, stop and join workers. Idempotent (also run by dtor).
    void Shutdown() {
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

private:
    void WorkerLoop() {
        for (;;) {
            std::packaged_task<R()> task;
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

    const std::size_t queue_capacity_;
    int task_timeout_ms_;

    mutable std::mutex mu_;
    std::condition_variable not_full_;
    std::condition_variable not_empty_;
    std::queue<std::packaged_task<R()>> queue_;  // guarded by mu_
    bool stopping_ = false;                       // guarded by mu_

    std::vector<std::thread> workers_;
};

}  // namespace cortrix::reranker
