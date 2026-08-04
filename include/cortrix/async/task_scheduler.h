#pragma once
#include <mutex>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <unordered_set>

#include "cortrix/async/task_info.h"
#include "cortrix/async/task_manager.h"
#include "cortrix/common/i_global_config.h"
#include "cortrix/common/result.h"

namespace cortrix::async {

/// TaskScheduler — per-doc_id mutual exclusion + Watcher debounce over the tasks
/// queue (F42 §4.2, topic 2). Sits on top of TaskManager: Enqueue applies the 5s
/// Watcher debounce (topic 2.2 C) and FIFO same-doc_id queueing (topic 2.1 A);
/// Dequeue hands a worker the oldest queued task whose doc_id is NOT already
/// being processed (topic 2.3 B — in-memory active_doc_ids_ set, same model as
/// F25 Q2 active_docs_); OnTaskCompleted releases the doc_id.
///
/// The active_doc_ids_ set is process-local (authoritative only within a process
/// lifetime); crash recovery rebuilds queued/processing state from the tasks
/// table (TaskManager::RequeueStaleProcessing / SweepZombies).
///
/// Standalone (D3): real and fully tested against an in-memory TaskManager. The
/// debounce window comes from IGlobalConfig f42.watcher_debounce_seconds (§4.0)
/// with a documented default; admin-API hot-reload (P08) → next Enqueue picks it up.
///
/// Thread-safe: Enqueue / Dequeue / OnTaskCompleted serialize on mutex_.
class TaskScheduler {
public:
    /// @param mgr    borrowed TaskManager (must outlive this scheduler)
    /// @param config borrowed IGlobalConfig for f42.watcher_debounce_seconds
    ///               (nullptr → kDefaultDebounceSeconds)
    TaskScheduler(TaskManager* mgr, const IGlobalConfig* config);

    /// topic 2 — submit a task. Applies the Watcher debounce (topic 2.2 C):
    ///   - same doc_id + same content_hash within the debounce window → merge:
    ///     return the existing task, no new row;
    ///   - same doc_id + different content_hash within the window → refresh
    ///     content_hash + reset progress (UpdateTaskForDebounce);
    ///   - otherwise → create a new queued task (topic 2.1 A: same doc_id already
    ///     processing is allowed; the per-doc_id mutex defers it at Dequeue).
    Result<TaskInfo> Enqueue(const SubmitRequest& req);

    /// topic 2.3 B — pop the oldest queued task whose (namespace, doc) pair is not
    /// reserved, mark it processing for `worker_id`, and reserve the pair. nullopt
    /// if the queue is empty or all queued docs are active.
    Result<std::optional<TaskInfo>> Dequeue(int worker_id);

    /// Release a (namespace, doc) reservation when its task reaches a terminal
    /// state (completed/failed/cancelled). Idempotent. Call from the worker after
    /// MarkCompleted / MarkFailed / MarkCancelled.
    void OnTaskCompleted(const std::string& namespace_id, const std::string& doc_id);

    /// Count of docs currently reserved as processing (test aid / observability).
    size_t ActiveDocCount() const;

    /// True iff (namespace_id, doc_id) is currently reserved (test aid).
    bool IsDocActive(const std::string& namespace_id, const std::string& doc_id) const;

    /// Hand back an input file this scheduler decided not to adopt.
    ///
    /// Enqueue can conclude that a caller-materialized `SubmitRequest.filepath`
    /// will never be read: a debounce merge keeps the existing task's own input, and
    /// a debounce refresh replaces the previous one. In both cases exactly one file
    /// stops having an owner at a point only the scheduler can see. Rather than let
    /// the scheduler know about any particular staging directory, it reports the
    /// path here; bootstrap wires this to the batch temp store, and leaving it unset
    /// (standalone/tests) means the caller owns its own cleanup.
    ///
    /// Arguments are (superseded_filepath, task_id_that_now_owns_the_identity).
    using UnadoptedInputReleaser =
        std::function<void(const std::string&, const std::string&)>;
    void SetUnadoptedInputReleaser(UnadoptedInputReleaser fn) {
        unadopted_input_releaser_ = std::move(fn);
    }

    /// f42.watcher_debounce_seconds default (§4.0, topic 2.2 C) when config absent.
    static constexpr int kDefaultDebounceSeconds = 5;

private:
    int DebounceSeconds() const;
    void ReleaseUnadoptedInput(const std::string& filepath,
                               const std::string& owner_task_id) const;

    /// Hash for the (namespace_id, doc_id) reservation key.
    struct NsDocHash {
        size_t operator()(const std::pair<std::string, std::string>& p) const {
            return std::hash<std::string>{}(p.first) ^
                   (std::hash<std::string>{}(p.second) << 1);
        }
    };

    TaskManager* mgr_;
    const IGlobalConfig* config_;
    UnadoptedInputReleaser unadopted_input_releaser_;

    mutable std::mutex mutex_;
    // topic 2.3 — in-flight (namespace, doc) reservations. Keyed on the pair because
    // a doc_id is only unique within its namespace.
    std::unordered_set<std::pair<std::string, std::string>, NsDocHash> active_docs_;
};

}  // namespace cortrix::async
