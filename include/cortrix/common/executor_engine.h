#pragma once
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace cortrix {

/// Shared bounded-queue thread pool. Consumed by the
/// reranker, ScatterGather, RAG-Fusion, and the Phase 2 multi-Unit router via
/// `#include "cortrix/common/executor_engine.h"`.
///
/// Submit<T>() enqueues a callable returning T and returns its future; N worker
/// threads drain the bounded queue. Submit blocks when the queue is full
/// (backpressure). QueueDepth()/ActiveWorkers() back the cortrix_executor_*
/// observability gauges. Non-copyable; Shutdown() (also run by the destructor)
/// drains queued work and joins the workers.
class ExecutorEngine {
public:
    /// @param workers     worker thread count (clamped to >= 1)
    /// @param queue_size  max queued-but-not-running tasks (clamped to >= 1)
    ExecutorEngine(int workers, int queue_size);
    ~ExecutorEngine();

    ExecutorEngine(const ExecutorEngine&) = delete;
    ExecutorEngine& operator=(const ExecutorEngine&) = delete;

    /// Enqueue a task and get its future. Blocks while the queue is at capacity.
    /// Must not be called after Shutdown() (the returned future would break).
    template <typename T>
    std::future<T> Submit(std::function<T()> task) {
        auto packaged = std::make_shared<std::packaged_task<T()>>(std::move(task));
        std::future<T> fut = packaged->get_future();
        Enqueue([packaged]() { (*packaged)(); });
        return fut;
    }

    int QueueDepth() const;     ///< tasks queued, not yet started (cortrix_executor_queue_depth)
    int ActiveWorkers() const;  ///< workers currently running a task (cortrix_executor_active_workers)

    /// Drain queued tasks, stop and join workers. Idempotent.
    void Shutdown();

private:
    void Enqueue(std::function<void()> job);
    void WorkerLoop();

    const std::size_t queue_capacity_;

    mutable std::mutex mu_;
    std::condition_variable not_full_;
    std::condition_variable not_empty_;
    std::queue<std::function<void()>> queue_;  // guarded by mu_
    bool shutdown_ = false;                     // guarded by mu_
    int active_workers_ = 0;                    // guarded by mu_

    std::vector<std::thread> workers_;
};

}  // namespace cortrix
