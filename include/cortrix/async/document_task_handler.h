#pragma once
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "cortrix/async/task_info.h"
#include "cortrix/async/task_manager.h"
#include "cortrix/async/task_scheduler.h"
#include "cortrix/async/worker_pool.h"
#include "cortrix/common/i_global_config.h"

namespace cortrix::async {

/// A fully-formed HTTP reply: status code + JSON body. The body for an async
/// success follows; the body for an error is the GEN-Agent 4-field
/// envelope (topic 5) produced by agent_friendly::ToJson(MakeTaskError(...)).
struct HttpResult {
    int status = 200;
    nlohmann::json body;
};

/// Parameters of a document submission, already parsed out of the multipart /
/// JSON request by the (deferred) httplib adapter. Keeping the handler free of
/// httplib makes the POST/GET/DELETE contract unit-testable standalone (S3); the
/// real route registration is integration.
struct SubmitParams {
    std::string namespace_id;
    std::string filename;
    std::string filepath;         ///< server-side temp path of the uploaded file
    std::string doc_id;           ///< Phase 1 content-hash-derived identity
    std::string content_hash;
    std::string trace_id;         ///< topic 6 — from traceparent header (implementation)
    int page_count = 0;           ///< pre-checked page estimate (sync/async decision)
    bool async_overflow = false;  ///< large-doc policy (topic 1.3): false = sync-only hard reject, true = async overflow path
};

/// DocumentTaskHandler — the POST(async submit) / GET(progress) / DELETE(cancel)
/// contract for async document tasks, independent of the HTTP
/// transport. Each method returns an HttpResult (status + GEN-Agent-shaped body).
///
/// Sync/async decision: page_count <= async_threshold_pages → the handler
/// reports a "sync handled elsewhere" result (the existing synchronous upload
/// path owns small docs; this handler only mints async tasks). Beyond the
/// threshold: async-overflow deployments enqueue an async task; sync-only → 403 CX_ERR_MAX_PAGES_EXCEEDED
/// (topic 1.3 A). page_count > async_max_pages → 403 for everyone.
///
/// Standalone: real decision/progress/cancel logic over the real
/// TaskScheduler/TaskManager, fully unit-tested. DEFERRED → integration: the httplib
/// route registration + multipart parsing + auth middleware + traceparent
/// extraction (this handler takes already-parsed SubmitParams / a task_id).
class DocumentTaskHandler {
public:
    /// @param scheduler borrowed (Enqueue path)
    /// @param mgr       borrowed (progress read / cancel)
    /// @param pool      borrowed; Notify() after a successful enqueue (nullptr ok
    ///                  in tests that don't run workers)
    /// @param config    borrowed; async.async_threshold_pages / async.ent_max_pages
    /// @param managed_input_dir where server-materialized task inputs live. A
    ///                  queued task cancels terminally inside CancelTask without
    ///                  ever reaching a worker, so TaskFinalizer never sees it and
    ///                  this is the only place that path can release its input.
    ///                  "" (default) disables the release, for wiring with no such
    ///                  directory.
    DocumentTaskHandler(TaskScheduler* scheduler, TaskManager* mgr,
                        WorkerPool* pool, const IGlobalConfig* config,
                        std::string managed_input_dir = "");

    /// POST /api/v1/documents — async submission decision + enqueue.
    /// 202 + {status:"processing", task_id, ...} when an async task is minted;
    /// 200 + {status:"sync_required"} when the doc is small enough for the sync
    /// path (caller routes to the synchronous uploader); 403 CX_ERR_MAX_PAGES_EXCEEDED
    /// for CE-over-threshold or anyone-over-ent-cap; 400 CX_ERR_INVALID_REQUEST
    /// on malformed params.
    HttpResult SubmitAsync(const SubmitParams& params);

    /// GET /api/v1/documents/tasks/{task_id}/progress — 200 + the progress
    /// body (incl. the 5-field meta), or 404 CX_ERR_TASK_NOT_FOUND.
    HttpResult GetProgress(const std::string& task_id);

    /// DELETE /api/v1/documents/tasks/{task_id} — 200 + the cancel-accepted
    /// body (queued→cancelled / processing→cancelling), 404 CX_ERR_TASK_NOT_FOUND,
    /// or 423 CX_ERR_TASK_CANCELLING on a repeat cancel of a cancelling/terminal task.
    HttpResult CancelTask(const std::string& task_id);

    /// async.* thresholds when config absent / malformed.
    static constexpr int kDefaultAsyncThresholdPages = 200;  // topic: sync threshold
    static constexpr int kDefaultAsyncMaxPages = 2000;        // topic 1.3

    /// Build the progress body (incl. meta) for a task row. Exposed for tests.
    static nlohmann::json BuildProgressBody(const TaskInfo& task);

private:
    int AsyncThresholdPages() const;
    int AsyncMaxPages() const;

    std::string managed_input_dir_;
    TaskScheduler* scheduler_;
    TaskManager* mgr_;
    WorkerPool* pool_;
    const IGlobalConfig* config_;
};

}  // namespace cortrix::async
