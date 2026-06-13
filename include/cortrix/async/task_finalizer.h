#pragma once
#include <chrono>
#include <string>

#include <nlohmann/json.hpp>

#include "cortrix/async/task_info.h"
#include "cortrix/async/task_manager.h"
#include "cortrix/common/status.h"

namespace cortrix::async {

/// TaskFinalizer — collapses the generic "terminal write + F42 task-level metric"
/// boilerplate that every ITaskHandler repeats at its terminal exits (F42 §4.1.1,
/// finalize ownership = handler, decision A 2026-06-09). Each handler still OWNS the
/// terminal *decision + content* (which outcome, which CX_ERR_* code, which
/// structured_data); this only unifies the mechanics: Mark* + RecordCompleted +
/// ObserveDuration. The framework (WorkerLoop) does NOT finalize — it dispatches and
/// releases the per-doc_id reservation; the returned Status is for the handler to
/// hand back for log/observe.
///
/// Why a string error_code (not F42ErrorCode) on Fail: handlers carry domain-specific
/// codes — DocumentProcessor uses the F42 enum (CX_ERR_PARSE_FAILED), F41AsyncWorker
/// uses CX_ERR_F41_* which is NOT in F42ErrorCode. A plain "CX_ERR_*" string keeps the
/// finalizer agnostic to any one handler's error namespace.
///
/// Duration: the handler times its own execution (it knows the boundary) and passes
/// t_start; the finalizer records it (cortrix_tasks_duration_seconds, End-to-end §6.bis).
class TaskFinalizer {
public:
    explicit TaskFinalizer(TaskManager* mgr);

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
    /// Returns F42Status(kTaskCancelling, task_id) (mirrors DocumentProcessor's cancel
    /// return — CX_ERR_TASK_CANCELLING / kAlreadyExists).
    Status Cancel(const TaskInfo& task,
                  std::chrono::steady_clock::time_point t_start);

private:
    TaskManager* mgr_;
};

}  // namespace cortrix::async
