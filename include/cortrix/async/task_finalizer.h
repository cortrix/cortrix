#pragma once
#include <chrono>
#include <string>

#include <nlohmann/json.hpp>

#include "cortrix/async/task_info.h"
#include "cortrix/async/task_manager.h"
#include "cortrix/common/status.h"

namespace cortrix::async {

/// TaskFinalizer — collapses the generic "terminal write + task-level metric"
/// boilerplate that every ITaskHandler repeats at its terminal exits
/// (finalize ownership = handler). Each handler still OWNS the
/// terminal *decision + content* (which outcome, which CX_ERR_* code, which
/// structured_data); this only unifies the mechanics: Mark* + RecordCompleted +
/// ObserveDuration. The framework (WorkerLoop) does NOT finalize — it dispatches and
/// releases the per-doc_id reservation; the returned Status is for the handler to
/// hand back for log/observe.
///
/// Why a string error_code (not TaskErrorCode) on Fail: handlers carry domain-specific
/// codes — DocumentProcessor uses the async enum (CX_ERR_PARSE_FAILED), DocSummaryAsyncWorker
/// uses CX_ERR_DOCSUMMARY_* which is NOT in TaskErrorCode. A plain "CX_ERR_*" string keeps the
/// finalizer agnostic to any one handler's error namespace.
///
/// Duration: the handler times its own execution (it knows the boundary) and passes
/// t_start; the finalizer records it (cortrix_tasks_duration_seconds, End-to-end).
class TaskFinalizer {
public:
    /// @param mgr borrowed TaskManager (terminal row write)
    /// @param managed_input_dir when non-empty, a task's `filepath` is deleted at
    ///        every terminal exit IF it sits directly in this directory. That is
    ///        the server-materialized batch input (server::BatchTempDir): the
    ///        server wrote it purely to feed the parser, so the task reaching a
    ///        terminal state is exactly when it stops being needed. Files anywhere
    ///        else are caller-owned (watcher sources, connector paths) and are
    ///        never touched, and handlers whose tasks carry no file are unaffected.
    ///        Empty (the default) disables the release entirely.
    /// @note  Deliberately best-effort: the terminal outcome is never failed
    ///        because an unlink failed. Bulk terminal transitions that bypass
    ///        handlers (SweepZombies / SweepTimedOut) never reach here at all, so
    ///        server::SweepOrphanedBatchInputs at startup is the required backstop,
    ///        not an optimization.
    explicit TaskFinalizer(TaskManager* mgr, std::string managed_input_dir = "");

    /// Success terminal: MarkCompleted(doc_id) + completed_total{kSuccess} + duration.
    /// Returns Ok (business success; a tasks-row persist failure is non-fatal — mirrors
    /// DocumentProcessor: a parse that succeeded stays OK even if the row was concurrently
    /// deleted). Fail/Cancel are likewise best-effort on the Mark* persist.
    Status Complete(const TaskInfo& task, const std::string& doc_id,
                    std::chrono::steady_clock::time_point t_start);

    /// Failure terminal: MarkFailed(error_code, error_msg, structured_data) +
    /// completed_total{kFailed} + duration. `error_code` is a "CX_ERR_*" string (any
    /// handler domain). Returns Status(kInternal, "error_code: error_msg") for log/observe.
    Status Fail(const TaskInfo& task, const std::string& error_code,
                const std::string& error_msg, const nlohmann::json& structured_data,
                std::chrono::steady_clock::time_point t_start);

    /// Cancel terminal: MarkCancelled + completed_total{kCancelled} + duration.
    /// Returns TaskStatus(kTaskCancelling, task_id) (mirrors DocumentProcessor's cancel
    /// return — CX_ERR_TASK_CANCELLING / kAlreadyExists).
    Status Cancel(const TaskInfo& task,
                  std::chrono::steady_clock::time_point t_start);

private:
    /// Delete the task's materialized input, if this task owns one. Called from
    /// every terminal exit (Complete / Fail / Cancel) after the row is written.
    void ReleaseManagedInput(const TaskInfo& task) const;

    TaskManager* mgr_;
    std::string managed_input_dir_;
};

}  // namespace cortrix::async
