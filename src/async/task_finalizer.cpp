#include "cortrix/async/task_finalizer.h"

#include <chrono>
#include <sstream>

#include "cortrix/async/f42_error.h"
#include "cortrix/async/f42_metrics.h"

#include <spdlog/spdlog.h>

namespace cortrix::async {
namespace {

double ElapsedSeconds(std::chrono::steady_clock::time_point t_start) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start)
        .count();
}

std::string StructuredDataKeys(const nlohmann::json& structured_data) {
    if (!structured_data.is_object()) return "<non-object>";
    std::ostringstream os;
    bool first = true;
    for (auto it = structured_data.begin(); it != structured_data.end(); ++it) {
        if (!first) os << ",";
        first = false;
        os << it.key();
    }
    return os.str();
}

}  // namespace

TaskFinalizer::TaskFinalizer(TaskManager* mgr) : mgr_(mgr) {}

Status TaskFinalizer::Complete(const TaskInfo& task, const std::string& doc_id,
                               std::chrono::steady_clock::time_point t_start) {
    // Best-effort persist: a successful business outcome is not undone by a tasks-row
    // persist failure (e.g. the row was concurrently deleted). Mirrors the original
    // DocumentProcessor contract — parse OK stays OK even if MarkCompleted NotFounds.
    mgr_->MarkCompleted(task.task_id, doc_id);
    const auto tt = static_cast<TaskType>(task.task_type);
    F42Metrics::Instance().RecordCompleted(tt, F42Metrics::CompletionStatus::kSuccess);
    F42Metrics::Instance().ObserveDuration(tt, ElapsedSeconds(t_start));
    return Status::Ok();
}

Status TaskFinalizer::Fail(const TaskInfo& task, const std::string& error_code,
                           const std::string& error_msg,
                           const nlohmann::json& structured_data,
                           std::chrono::steady_clock::time_point t_start) {
    // Best-effort persist (mirrors DocumentProcessor: the business outcome is what the
    // returned Status reports; a Mark* DB failure is logged via the store layer).
    mgr_->MarkFailed(task.task_id, error_code, error_msg, structured_data.dump());
    spdlog::warn(
        "F42 TaskFinalizer: task failed task_id={} task_type={} namespace_id={} doc_id={} "
        "error_code={} error_msg={} structured_data_keys={}",
        task.task_id, task.task_type, task.namespace_id, task.doc_id, error_code,
        error_msg, StructuredDataKeys(structured_data));
    const auto tt = static_cast<TaskType>(task.task_type);
    F42Metrics::Instance().RecordCompleted(tt, F42Metrics::CompletionStatus::kFailed);
    F42Metrics::Instance().ObserveDuration(tt, ElapsedSeconds(t_start));
    return Status(StatusCode::kInternal, error_code + ": " + error_msg);
}

Status TaskFinalizer::Cancel(const TaskInfo& task,
                             std::chrono::steady_clock::time_point t_start) {
    mgr_->MarkCancelled(task.task_id);
    const auto tt = static_cast<TaskType>(task.task_type);
    F42Metrics::Instance().RecordCompleted(tt, F42Metrics::CompletionStatus::kCancelled);
    F42Metrics::Instance().ObserveDuration(tt, ElapsedSeconds(t_start));
    return F42Status(F42ErrorCode::kTaskCancelling, task.task_id);
}

}  // namespace cortrix::async
