#pragma once
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace cortrix::reranker {

/// RerankerThreadPool — F02's OWN bounded-queue thread pool (F02 §3.1 / topic 3.3).
///
/// D3 standalone (F02-3 → D3.5): F02 deliberately keeps its own pool this phase;
/// sharing the global cortrix::ExecutorEngine ("ThreadPool→ExecutorEngine sharing",
/// F02-3 hook) is cross-Feature integration deferred to D3.5. The interface here
/// mirrors the spec (`std::future<float> Submit(std::function<float()>)`) so the
/// later swap to ExecutorEngine keeps `reranker.workers` GUC semantics.
///
/// Behavior (topic 1.2 / 1.3 / 3.3):
///   - N (default 4) resident worker threads drain a bounded FIFO queue.
///   - Submit() BLOCKS when the queue is full (backpressure; no dropped request).
///   - task_timeout_ms is the per-task budget enforced by the CALLER via
///     SubmitWaitFor()/std::future::wait_for (the pool stores it so callers don't
///     restate it). A timed-out task is abandoned to the worker (its future is
///     dropped); the caller treats it as score=0 (S3.4) + records the timeout.
class RerankerThreadPool {
public:
    /// @param num_workers     resident worker count (clamped to >= 1)
    /// @param queue_size      max queued-but-not-running tasks (clamped to >= 1)
    /// @param task_timeout_ms per-task budget surfaced to callers (stored only)
    RerankerThreadPool(int num_workers, int queue_size, int task_timeout_ms);
    ~RerankerThreadPool();

    RerankerThreadPool(const RerankerThreadPool&) = delete;
    RerankerThreadPool& operator=(const RerankerThreadPool&) = delete;

    /// Enqueue a scoring task; returns its future. BLOCKS while the queue is full
    /// (FIFO). Must not be called after Shutdown().
    std::future<float> Submit(std::function<float()> task);

    /// Submit + wait up to task_timeout_ms. Returns {value, true} on completion,
    /// or {0.0f, false} on timeout (caller maps timeout → score=0 + metric, S3.4).
    std::pair<float, bool> SubmitWaitFor(std::function<float()> task);

    int num_workers() const { return static_cast<int>(workers_.size()); }
    int task_timeout_ms() const { return task_timeout_ms_; }
    std::size_t queue_capacity() const { return queue_capacity_; }

    /// Current queue depth (tasks queued, not yet started) —
    /// cortrix_reranker_queue_depth_current gauge source.
    int QueueDepth() const;

    /// Drain queued tasks, stop and join workers. Idempotent (also run by dtor).
    void Shutdown();

private:
    void WorkerLoop();

    const std::size_t queue_capacity_;
    int task_timeout_ms_;

    mutable std::mutex mu_;
    std::condition_variable not_full_;
    std::condition_variable not_empty_;
    std::queue<std::packaged_task<float()>> queue_;  // guarded by mu_
    bool stopping_ = false;                           // guarded by mu_

    std::vector<std::thread> workers_;
};

}  // namespace cortrix::reranker
