#include "cortrix/async/worker_pool.h"

#include <chrono>
#include <exception>

#include <spdlog/spdlog.h>

#include "cortrix/async/f42_error.h"
#include "cortrix/async/f42_metrics.h"

namespace cortrix::async {

WorkerPool::WorkerPool(TaskScheduler* scheduler, const IGlobalConfig* config)
    : scheduler_(scheduler), config_(config) {}

void WorkerPool::RegisterHandler(int task_type, ITaskHandler* handler) {
    handlers_[task_type] = handler;
}

WorkerPool::~WorkerPool() { Stop(); }

int WorkerPool::ConfiguredPoolSize() const {
    if (config_) {
        auto r = config_->GetInt("f42.worker_pool_size");
        if (r.ok()) return r.value();
    }
    return kDefaultPoolSize;
}

int WorkerPool::ParserMaxConcurrent() const {
    if (config_) {
        auto r = config_->GetInt("f06.parser_max_concurrent");
        if (r.ok()) return r.value();
    }
    return kDefaultParserMaxConcurrent;
}

Status WorkerPool::Start() {
    if (started_) return Status::Ok();

    const int pool_size = ConfiguredPoolSize();
    const int parser_max = ParserMaxConcurrent();
    // topic 1.2 B — startup enforce: worker pool must not exceed F06's parser
    // concurrency. The spec LOG_FATAL+exit(1)s; here we fail fast with a code so
    // the host (and tests) can handle it without aborting the process.
    if (pool_size > parser_max) {
        nlohmann::json sd = {
            {"rules_violated",
             {"f42.worker_pool_size must be <= f06.parser_max_concurrent"}},
            {"worker_pool_size", pool_size},
            {"parser_max_concurrent", parser_max},
        };
        return F42Status(F42ErrorCode::kInvalidRequest,
                         "f42.worker_pool_size (" + std::to_string(pool_size) +
                             ") > f06.parser_max_concurrent (" +
                             std::to_string(parser_max) + ")");
    }
    if (pool_size < 1) {
        return F42Status(F42ErrorCode::kInvalidRequest,
                         "f42.worker_pool_size must be >= 1");
    }

    stop_.store(false);
    workers_.reserve(static_cast<size_t>(pool_size));
    for (int i = 0; i < pool_size; ++i) {
        workers_.emplace_back([this, i] { WorkerLoop(i + 1); });  // worker_id 1-based
    }
    started_ = true;
    spdlog::info("F42 WorkerPool started with {} workers (parser_max={})",
                 pool_size, parser_max);
    return Status::Ok();
}

void WorkerPool::Stop() {
    if (!started_) return;
    stop_.store(true);
    cv_.notify_all();
    for (auto& t : workers_) {
        if (t.joinable()) t.join();
    }
    workers_.clear();
    started_ = false;
}

void WorkerPool::Notify() { cv_.notify_one(); }

void WorkerPool::WorkerLoop(int worker_id) {
    while (!stop_.load()) {
        auto dq = scheduler_->Dequeue(worker_id);
        if (dq.ok() && dq.value().has_value()) {
            const TaskInfo& task = *dq.value();
            // Dispatch by task_type to the registered handler (F42 §4.1 / F41 §10.2).
            // Process outside any lock; ProcessTask drives progress + finalize.
            //
            // R2-C1 — a handler MUST NOT throw (it returns a CX_ERR_* Status on
            // failure), but if one does we catch it here so the misbehaving task
            // fails in isolation. An uncaught exception would escape the thread
            // entry point and call std::terminate(), crashing the whole process.
            // OnTaskCompleted (below) then runs UNCONDITIONALLY on every path —
            // normal return, no-handler, caught exception — because the catch
            // blocks swallow the throw and let control fall through to it. Skipping
            // it on the throw path would leak the per-doc_id reservation: the
            // doc_id would stay in active_doc_ids_ until process restart and that
            // doc would forever report in-progress (no worker can re-dequeue it).
            auto it = handlers_.find(task.task_type);
            if (it != handlers_.end() && it->second != nullptr) {
                try {
                    it->second->ProcessTask(task);
                } catch (const std::exception& e) {
                    spdlog::error(
                        "F42 WorkerPool: handler threw for task {} (task_type {}): "
                        "{} — task failed, worker continues",
                        task.task_id, task.task_type, e.what());
                    F42Metrics::Instance().RecordCompleted(
                        static_cast<TaskType>(task.task_type),
                        F42Metrics::CompletionStatus::kFailed);
                } catch (...) {
                    spdlog::error(
                        "F42 WorkerPool: handler threw a non-std exception for task "
                        "{} (task_type {}) — task failed, worker continues",
                        task.task_id, task.task_type);
                    F42Metrics::Instance().RecordCompleted(
                        static_cast<TaskType>(task.task_type),
                        F42Metrics::CompletionStatus::kFailed);
                }
            } else {
                spdlog::error("F42 WorkerPool: no handler for task_type {} (task {})",
                              task.task_type, task.task_id);
            }
            // Release the per-doc_id reservation regardless of terminal outcome
            // (success, failure, or a caught handler exception above).
            scheduler_->OnTaskCompleted(task.doc_id);
            continue;  // immediately try for the next queued task
        }
        // Empty queue (or all docs active) → wait for a Notify()/Stop(), with a
        // bounded fallback so a task that became eligible (a peer released its
        // doc_id) is still picked up even without an explicit Notify.
        std::unique_lock<std::mutex> lock(cv_mu_);
        cv_.wait_for(lock, std::chrono::milliseconds(50),
                     [this] { return stop_.load(); });
    }
}

}  // namespace cortrix::async
