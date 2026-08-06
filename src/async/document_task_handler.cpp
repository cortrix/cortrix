#include "cortrix/async/document_task_handler.h"

#include <utility>

#include "cortrix/async/managed_input.h"

#include "cortrix/agent_friendly/error.h"
#include "cortrix/async/task_error.h"
#include "cortrix/async/task_metrics.h"

namespace cortrix::async {

namespace {

/// Build the GEN-Agent error HttpResult (topic 5): http status from the §6.2
/// registry, body = {"error": agent_friendly::ToJson(MakeTaskError(...))}. The
/// single error exit for every handler so the wrapped envelope (R2-M8: fields
/// under the "error" key, matching WriteJsonError + the 16 other ToJson call
/// sites) is emitted uniformly.
HttpResult ErrorResult(TaskErrorCode code, nlohmann::json structured_data,
                       const std::string& message = "") {
    HttpResult r;
    r.status = TaskErrorHttpStatus(code);
    r.body["error"] = agent_friendly::ToJson(MakeTaskError(code, std::move(structured_data), message));
    return r;
}

}  // namespace

DocumentTaskHandler::DocumentTaskHandler(TaskScheduler* scheduler, TaskManager* mgr,
                                         WorkerPool* pool, const IGlobalConfig* config,
                                         std::string managed_input_dir)
    : managed_input_dir_(std::move(managed_input_dir)),
      scheduler_(scheduler), mgr_(mgr), pool_(pool), config_(config) {}

int DocumentTaskHandler::AsyncThresholdPages() const {
    if (config_) {
        auto r = config_->GetInt("async.async_threshold_pages");
        if (r.ok()) return r.value();
    }
    return kDefaultAsyncThresholdPages;
}

int DocumentTaskHandler::AsyncMaxPages() const {
    if (config_) {
        auto r = config_->GetInt("async.async_max_pages");
        if (r.ok()) return r.value();
    }
    return kDefaultAsyncMaxPages;
}

nlohmann::json DocumentTaskHandler::BuildProgressBody(const TaskInfo& task) {
    // §6.3 Progress API body + topic 5 meta (5 fields). retryable_if_fail mirrors
    // the persisted error_code's registry retryability when failed, else true.
    nlohmann::json body;
    body["task_id"] = task.task_id;
    body["status"] = task.status;
    body["total_pages"] = task.total_pages;
    body["processed_pages"] = task.processed_pages;
    body["failed_pages"] = task.failed_pages;
    body["progress_pct"] = task.progress_pct;
    body["eta_seconds"] = task.eta_seconds;
    body["started_at"] = task.started_at;
    body["updated_at"] = task.updated_at;
    body["doc_id"] = task.doc_id.empty() ? nlohmann::json(nullptr)
                                         : nlohmann::json(task.doc_id);
    body["trace_id"] = task.trace_id.empty() ? nlohmann::json(nullptr)
                                             : nlohmann::json(task.trace_id);

    // topic 5 — GEN-Agent error transparency: a failed task MUST surface its
    // machine-readable error_code (+ msg + structured context) so an Agent can act on
    // the `inspect_error_code` hint below; without it the hint points at nothing.
    // Present-and-null on non-failed states keeps the schema stable.
    const bool failed = (task.status == task_status::kFailed);
    body["error_code"] = (failed && !task.error_code.empty())
        ? nlohmann::json(task.error_code) : nlohmann::json(nullptr);
    body["error_msg"] = (failed && !task.error_msg.empty())
        ? nlohmann::json(task.error_msg) : nlohmann::json(nullptr);
    // structured_data is persisted as a serialized JSON string; re-inflate to an
    // object so the Agent gets structured fields (null if absent / unparseable).
    if (failed && !task.structured_data.empty()) {
        body["structured_data"] =
            nlohmann::json::parse(task.structured_data, nullptr, /*allow_exceptions=*/false);
        if (body["structured_data"].is_discarded()) body["structured_data"] = nullptr;
    } else {
        body["structured_data"] = nlohmann::json(nullptr);
    }

    // agent_decision_hint: a machine-readable next action (GEN-Agent autonomy).
    std::string hint;
    if (task.status == task_status::kQueued || task.status == task_status::kProcessing) {
        hint = "poll_again_in_5s";
    } else if (task.status == task_status::kCompleted) {
        hint = "done_fetch_doc";
    } else if (task.status == task_status::kCancelling) {
        hint = "cancel_in_progress_poll";
    } else if (task.status == task_status::kCancelled) {
        hint = "cancelled_stop";
    } else {  // failed
        hint = "inspect_error_code";
    }

    nlohmann::json meta;
    meta["worker_id"] = task.worker_id;
    meta["current_phase"] = task.current_phase;
    meta["retryable_if_fail"] = true;  // async parse path is retryable by default
    meta["agent_decision_hint"] = hint;
    // estimated_finish_at: filled when a real ETA exists (eta_seconds >= 0);
    // otherwise null (the streaming-ETA clock is wired in D3.5, §4.5 note).
    meta["estimated_finish_at"] = nullptr;
    body["meta"] = meta;
    return body;
}

HttpResult DocumentTaskHandler::SubmitAsync(const SubmitParams& params) {
    // 400 — minimal validation (namespace + a server-side file path required).
    if (params.namespace_id.empty() || params.filepath.empty() ||
        params.filename.empty()) {
        return ErrorResult(
            TaskErrorCode::kInvalidRequest,
            {{"rules_violated",
              {"namespace_id, filename and filepath are required"}}},
            "missing required submission fields");
    }

    const int threshold = AsyncThresholdPages();
    const int hard_cap = AsyncMaxPages();

    // §2.2 / scenario table — over the async hard cap → reject for everyone.
    if (params.page_count > hard_cap) {
        return ErrorResult(TaskErrorCode::kMaxPagesExceeded,
                           {{"max_pages", hard_cap},
                            {"actual_pages", params.page_count},
                            {"async_overflow", params.async_overflow}},
                           "document exceeds the maximum page limit");
    }

    // §2.2 — small docs go through the synchronous upload path (owned elsewhere);
    // this async handler only mints tasks beyond the threshold.
    if (params.page_count <= threshold) {
        HttpResult r;
        r.status = 200;
        r.body = {{"status", "sync_required"},
                  {"reason", "page_count within sync threshold"},
                  {"threshold_pages", threshold}};
        return r;
    }

    // Over threshold but within the hard cap (topic 1.3 A): sync-only
    // deployments reject; async-overflow deployments enqueue.
    if (!params.async_overflow) {
        return ErrorResult(TaskErrorCode::kMaxPagesExceeded,
                           {{"max_pages", threshold},
                            {"actual_pages", params.page_count},
                            {"async_overflow", params.async_overflow}},
                           "document exceeds the synchronous page threshold");
    }

    // Async overflow path — enqueue (scheduler applies debounce/FIFO).
    SubmitRequest sreq;
    sreq.namespace_id = params.namespace_id;
    sreq.filename = params.filename;
    sreq.filepath = params.filepath;
    sreq.doc_id = params.doc_id;
    sreq.content_hash = params.content_hash;
    sreq.trace_id = params.trace_id;
    sreq.total_pages = params.page_count;
    sreq.task_type = kTaskDocParse;

    auto enq = scheduler_->Enqueue(sreq);
    if (!enq.ok()) {
        // Surface a transient service error (tasks_db) — retryable per §6.2.
        return ErrorResult(TaskErrorCode::kServiceUnavailable,
                           {{"component", "tasks_db"}}, enq.status().message());
    }
    if (pool_) pool_->Notify();  // wake a worker to pick it up

    // 202 TASK_SUBMITTED success body (§6.2 row 1).
    HttpResult r;
    r.status = 202;
    r.body = {{"status", "processing"},
              {"task_id", enq.value().task_id},
              {"eta_seconds", enq.value().eta_seconds},
              {"accepted_at", enq.value().created_at}};
    return r;
}

HttpResult DocumentTaskHandler::GetProgress(const std::string& task_id) {
    auto got = mgr_->GetTask(task_id);
    if (!got.ok()) {
        return ErrorResult(TaskErrorCode::kTaskNotFound, nlohmann::json::object(),
                           "no such task: " + task_id);
    }
    HttpResult r;
    r.status = 200;
    r.body = BuildProgressBody(got.value());
    return r;
}

HttpResult DocumentTaskHandler::CancelTask(const std::string& task_id) {
    TaskInfo out;
    Status s = mgr_->RequestCancel(task_id, &out);
    if (s.ok()) {
        // §6.bis cortrix_tasks_cancel_total{phase}: derive the lifecycle phase from
        // the post-transition status — a queued task cancels terminally (pre_dequeue),
        // a processing task is signalled to its checkpoint (mid_processing). The
        // post_chunk_idx phase (cancel after a chunk-index write) is only reachable
        // once the write-coordinator BeginWrite integration lands.
        TaskMetrics::Instance().RecordCancel(
            out.status == task_status::kCancelled
                ? TaskMetrics::CancelPhase::kPreDequeue
                : TaskMetrics::CancelPhase::kMidProcessing);
        // A queued task cancels terminally right here (pre_dequeue) — no worker ever
        // dispatches it, so TaskFinalizer never runs and this is the only place its
        // materialized input can be released. A processing task instead reaches
        // `cancelling` and is released by the finalizer at its checkpoint.
        if (out.status == task_status::kCancelled) {
            ReleaseManagedInput(managed_input_dir_, out, mgr_);
        }
        // §4.3 — 200 + GEN-Agent cancel-accepted body.
        HttpResult r;
        r.status = 200;
        r.body = {
            {"task_id", task_id},
            {"cancel_requested", true},
            {"current_status", out.status},
            {"estimated_cancellation_in_seconds",
             out.status == task_status::kCancelling ? 5 : 0},
            {"message", "Cancel signal sent. Poll GET /api/v1/documents/tasks/{id}/progress"},
        };
        return r;
    }
    // RequestCancel maps NotFound → CX_ERR_TASK_NOT_FOUND, AlreadyExists →
    // CX_ERR_TASK_CANCELLING (repeat cancel of cancelling/terminal, §6.2 423).
    if (s.code() == StatusCode::kNotFound) {
        return ErrorResult(TaskErrorCode::kTaskNotFound, nlohmann::json::object(),
                           "no such task: " + task_id);
    }
    // current_status for the 423 structured_data — best-effort re-read.
    std::string cur;
    auto got = mgr_->GetTask(task_id);
    if (got.ok()) cur = got.value().status;
    return ErrorResult(TaskErrorCode::kTaskCancelling,
                       {{"task_id", task_id}, {"current_status", cur}},
                       "task is already cancelling or terminal");
}

}  // namespace cortrix::async
