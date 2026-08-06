#pragma once
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "cortrix/async/task_handler.h"
#include "cortrix/async/task_scheduler.h"
#include "cortrix/common/i_global_config.h"
#include "cortrix/common/status.h"

namespace cortrix::async {

/// WorkerPool — N resident worker threads draining the async task queue.
/// Each worker loops: TaskScheduler::Dequeue → DocumentProcessor::
/// ProcessTask → TaskScheduler::OnTaskCompleted(doc_id). When the queue is empty
/// a worker blocks on a condition variable until Notify() (a new Enqueue) or Stop().
///
/// Sizing (topic 1.1 / 1.2 B): worker count = f42.worker_pool_size (default 2),
/// and Start() ENFORCES worker_pool_size ≤ f06.parser_max_concurrent — exceeding
/// it is a fatal misconfiguration (Start returns CX_ERR_INVALID_REQUEST rather
/// than the spec's LOG_FATAL/exit, so the server/tests fail fast without aborting
/// the process under test). Hot-reload of the pool size via the admin API is
/// DEFERRED; Phase 1 size is fixed at Start().
///
/// Standalone (D3): real threads + real Dequeue/ProcessTask loop, tested against
/// an in-memory TaskManager + injected stub parser. DEFERRED → D3.5: real server
/// lifecycle wiring (the pool is owned/started by the document service at boot).
///
/// Thread-safe lifecycle: Start/Stop are not concurrent with each other (caller
/// serializes); Notify() is safe from any thread.
class WorkerPool {
public:
    /// @param scheduler borrowed TaskScheduler (the queue + per-doc_id mutex)
    /// @param config    borrowed IGlobalConfig for f42.worker_pool_size /
    ///                  f06.parser_max_concurrent (nullptr → defaults)
    /// Register per-task_type handlers via RegisterHandler() before Start().
    WorkerPool(TaskScheduler* scheduler, const IGlobalConfig* config);

    /// Register the handler for one TaskType (borrowed; must outlive the pool).
    /// Call before Start(); the last registration for a task_type wins. A dequeued
    /// task whose task_type has no handler is logged + its reservation released
    /// (a wiring error — every enqueued type should have a registered handler).
    void RegisterHandler(int task_type, ITaskHandler* handler);
    ~WorkerPool();

    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;

    /// Start `worker_pool_size` worker threads after enforcing the parser-cap
    /// invariant (topic 1.2). Returns CX_ERR_INVALID_REQUEST (InvalidArgument) if
    /// worker_pool_size > f06.parser_max_concurrent. No-op if already started.
    Status Start();

    /// Stop and join all workers (drains nothing new; in-flight ProcessTask runs
    /// to its checkpoint). Idempotent; also run by the destructor.
    void Stop();

    /// Wake one idle worker to poll the queue (call after TaskScheduler::Enqueue).
    void Notify();

    /// Configured worker count (post-Start; 0 before).
    int worker_count() const { return static_cast<int>(workers_.size()); }

    /// The size the pool WILL/DID start with, from config (topic 1.1). Available
    /// before Start() for the enforce check + tests.
    int ConfiguredPoolSize() const;

    /// f06.parser_max_concurrent as seen by the enforce check (topic 1.2).
    int ParserMaxConcurrent() const;

    /// f42.* / f06.* defaults (§4.0) when config absent / malformed.
    static constexpr int kDefaultPoolSize = 2;            ///< f42.worker_pool_size
    static constexpr int kDefaultParserMaxConcurrent = 4; ///< f06.parser_max_concurrent

private:
    void WorkerLoop(int worker_id);

    TaskScheduler* scheduler_;
    const IGlobalConfig* config_;
    std::unordered_map<int, ITaskHandler*> handlers_;  // task_type → handler (borrowed)

    std::vector<std::thread> workers_;
    std::mutex cv_mu_;
    std::condition_variable cv_;
    std::atomic<bool> stop_{false};
    bool started_ = false;
};

}  // namespace cortrix::async
