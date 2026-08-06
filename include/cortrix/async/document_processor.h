#pragma once
#include <functional>
#include <stdexcept>
#include <string>

#include "cortrix/async/task_finalizer.h"
#include "cortrix/async/task_handler.h"
#include "cortrix/async/task_info.h"
#include "cortrix/async/task_manager.h"
#include "cortrix/common/i_global_config.h"
#include "cortrix/common/status.h"
#include "cortrix/spc/parser_factory.h"

namespace cortrix { class SPCManager; }  // optional post-parse SPC entry

namespace cortrix::async {

/// Thrown from the on_page_progress callback when the task's cancel flag is
/// observed at the chunk-index checkpoint (topic 3.1 B). Unwinds out of
/// DocumentParserFactory::ParseDocument so the worker can finalize as cancelled.
class CancellationException : public std::runtime_error {
public:
    explicit CancellationException(std::string task_id)
        : std::runtime_error("task cancelled: " + task_id),
          task_id_(std::move(task_id)) {}
    const std::string& task_id() const { return task_id_; }

private:
    std::string task_id_;
};

/// DocumentProcessor — runs one async task through the document parser, persisting
/// per-page progress and honoring cancellation.
///
/// It wires an on_page_progress callback that, per page: (1) checks the task's
/// cancel_requested flag at the chunk-index checkpoint (topic 3.1 B) and throws
/// CancellationException if set; (2) updates processed/total/failed/progress/eta
/// + current_phase=parsing and persists via TaskManager::UpdateProgress.
///
/// Cancellation polling reads the live cancel flag through a pluggable
/// CancelChecker so tests can drive it deterministically without a DB write race;
/// production passes a checker backed by TaskManager::GetTask.
///
/// Standalone: the parser call + progress + cancel checkpoint are real
/// (tested against an injected stub IDocumentParser, no Python). DEFERRED → integration:
/// the post-parse SPC pipeline (Enricher/Chunk/Index stages) + BeginWrite /
/// RollbackCallback on cancel-after-write + real TraceContext threading. Until
/// then ProcessTask stops at "parsed", and on cancel simply finalizes cancelled
/// (no chunk rollback needed because nothing was written downstream yet).
class DocumentProcessor : public ITaskHandler {
public:
    /// Reads the current cancel_requested for a task_id. Returns true to abort.
    using CancelChecker = std::function<bool(const std::string& task_id)>;

    /// @param mgr     borrowed TaskManager (progress persistence + finalization)
    /// @param factory borrowed parser factory (the real parse engine)
    /// @param config  borrowed IGlobalConfig for async.async_max_pages (parser cap);
    ///                nullptr → kDefaultAsyncMaxPages
    /// @param cancel_checker how to poll the cancel flag (defaults to GetTask)
    /// @param spc_mgr  optional SPC entry: when set, a successful
    ///                 parse is handed to SPCManager::ProcessParsedDoc (Chunk→…→write);
    ///                 nullptr = parse-only standalone (SPC deferred).
    /// @param managed_input_dir forwarded to TaskFinalizer: server-materialized batch
    ///                 inputs living in this directory are released when the task
    ///                 reaches any terminal state. Pass server::BatchTempDir(data_dir)
    ///                 in production; "" (default) keeps the old retain-forever
    ///                 behavior for standalone/test wiring that has no such dir.
    DocumentProcessor(TaskManager* mgr, spc::DocumentParserFactory* factory,
                      const IGlobalConfig* config,
                      CancelChecker cancel_checker = nullptr,
                      cortrix::SPCManager* spc_mgr = nullptr,
                      std::string managed_input_dir = "");

    /// Process `task` (already marked processing): parse with progress +
    /// cancel checkpoint, then finalize. On success → MarkCompleted(doc_id). On
    /// CancellationException → MarkCancelled. On a parse error → MarkFailed with
    /// the mapped CX_ERR_* code. Returns the terminal Status (Ok on completed;
    /// the error Status otherwise) so the caller (worker) can log/observe.
    Status ProcessTask(const TaskInfo& task) override;

    /// async.async_max_pages default (topic 1.3 — async-path page cap).
    static constexpr int kDefaultAsyncMaxPages = 2000;

private:
    int AsyncMaxPages() const;

    TaskManager* mgr_;
    spc::DocumentParserFactory* factory_;
    const IGlobalConfig* config_;
    CancelChecker cancel_checker_;
    TaskFinalizer finalizer_;  // terminal write + task metric collapse
    cortrix::SPCManager* spc_mgr_;  // optional post-parse SPC entry
};

}  // namespace cortrix::async
