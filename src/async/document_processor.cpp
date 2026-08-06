#include "cortrix/async/document_processor.h"

#include <algorithm>
#include <chrono>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "cortrix/async/task_error.h"
#include "cortrix/spc/parser.h"
#include "cortrix/spc/parser_errors.h"
#include "cortrix/spc/spc_manager.h"
#include "cortrix/spc/spc_task.h"

namespace cortrix::async {

namespace {
// Async TaskInfo lacks mime_type; derive just enough from the
// filename for SPCRouter::InferBlockType (which only distinguishes image scans from files).
std::string InferMimeFromFilename(const std::string& fn) {
    auto ends = [&](const char* s) {
        const size_t n = std::char_traits<char>::length(s);
        return fn.size() >= n && fn.compare(fn.size() - n, n, s) == 0;
    };
    if (ends(".png")) return "image/png";
    if (ends(".jpg") || ends(".jpeg")) return "image/jpeg";
    if (ends(".tiff") || ends(".tif")) return "image/tiff";
    if (ends(".bmp")) return "image/bmp";
    return "application/pdf";  // non-image → kBlockFile
}
}  // namespace

DocumentProcessor::DocumentProcessor(TaskManager* mgr,
                                     spc::DocumentParserFactory* factory,
                                     const IGlobalConfig* config,
                                     CancelChecker cancel_checker,
                                     cortrix::SPCManager* spc_mgr,
                                     std::string managed_input_dir)
    : mgr_(mgr),
      factory_(factory),
      config_(config),
      cancel_checker_(std::move(cancel_checker)),
      finalizer_(mgr, std::move(managed_input_dir)),
      spc_mgr_(spc_mgr) {
    if (!cancel_checker_) {
        // Default: poll the persisted cancel_requested flag via the manager.
        cancel_checker_ = [this](const std::string& task_id) {
            auto t = mgr_->GetTask(task_id);
            return t.ok() && t.value().cancel_requested;
        };
    }
}

int DocumentProcessor::AsyncMaxPages() const {
    if (config_) {
        auto r = config_->GetInt("async.async_max_pages");
        if (r.ok()) return r.value();
    }
    return kDefaultAsyncMaxPages;
}

Status DocumentProcessor::ProcessTask(const TaskInfo& task) {
    // Task metrics — record the terminal outcome + end-to-end duration,
    // keyed on the task's task_type. The /metrics endpoint wiring is integration; the
    // recorder + these feed points are the piece.
    const auto t_start = std::chrono::steady_clock::now();

    // Local progress accumulator mirrored into the tasks table each page.
    TaskInfo prog = task;
    prog.current_phase = task_phase::kParsing;  // topic 5 — only the parse phase is wired in
    prog.failed_pages.clear();

    spc::ParserOptions opts;
    opts.max_pages = AsyncMaxPages();  // topic 1.3 — async-path page cap (default 2000)

    // topic 3.1 B — chunk-index checkpoint: before processing each page, observe
    // the cancel flag and unwind if it is set. topic 5 — persist per-page progress.
    opts.on_page_progress = [this, &prog](int page, int total, bool ok) {
        if (cancel_checker_ && cancel_checker_(prog.task_id)) {
            throw CancellationException(prog.task_id);
        }
        prog.processed_pages = page;
        prog.total_pages = total;
        if (!ok) prog.failed_pages.push_back(page);
        prog.progress_pct = total > 0 ? 100.0f * static_cast<float>(page) /
                                            static_cast<float>(total)
                                      : 0.0f;
        // ETA: remaining pages × observed per-page time is wired in S3/integration with a
        // real clock; records -1 (unknown) until the streaming driver exists.
        prog.eta_seconds = -1;
        Status s = mgr_->UpdateProgress(prog);
        if (!s.ok()) {
            spdlog::warn("progress persist failed for {}: {}", prog.task_id,
                         s.message());
        }
    };

    spc::ParsedDoc result;
    try {
        result = factory_->ParseDocument(task.filepath, opts);
    } catch (const CancellationException&) {
        // The parser subprocess is not force-killed; the current page
        // finished naturally and the checkpoint threw. Finalize as cancelled.
        // DEFERRED: if a downstream BeginWrite had already landed
        // chunks, invoke RollbackCallback here (nothing is written in, so the
        // cancel is clean).
        return finalizer_.Cancel(task, t_start);
    }

    if (!result.ok()) {
        // Map the ParserErrorCode → CX_ERR_PARSE_FAILED + GEN-Agent structured_data
        // (topic 5 error persistence); the finalizer persists via MarkFailed + metric.
        nlohmann::json sd = {
            {"task_id", task.task_id},
            {"page_number", result.failed_pages.empty() ? -1 : result.failed_pages.front()},
            {"parser_error", result.error_msg},
        };
        return finalizer_.Fail(task, TaskErrorCodeString(TaskErrorCode::kParseFailed),
                               result.error_msg, sd, t_start);
    }

    // Re-check cancel after parsing, before declaring completion: a cancel that
    // arrived on the last page (so the checkpoint didn't fire again) still wins.
    if (cancel_checker_ && cancel_checker_(task.task_id)) {
        return finalizer_.Cancel(task, t_start);
    }

    // If wired to SPC, hand the parsed doc to the shared post-parse
    // stages (chunk → META → enrich → embed → assemble → write) via SPCManager; else
    // (standalone) finalize parse-only with the task's content-hash-derived doc_id.
    if (spc_mgr_) {
        SPCTask spc_task;
        spc_task.doc_id = task.doc_id;
        spc_task.namespace_name = task.namespace_id;
        // source_path is the document's identity/display name on the row (the list
        // surfaces it as "filename"), NOT the parse input — parsing already ran on
        // task.filepath above. Use the caller's original filename so batch-ingested
        // docs show their real name (mirroring upload_handler), instead of the internal
        // batch_tmp materialize path. Fall back to filepath if no filename was given.
        spc_task.source_path = task.filename.empty() ? task.filepath : task.filename;
        spc_task.content_hash = task.content_hash;
        spc_task.processing_level = 3;  // L3 full processing for async large docs
        spc_task.source_type = "file";
        spc_task.mime_type = InferMimeFromFilename(task.filename);
        // Carry caller-supplied document metadata (e.g. an external corpus id) so it
        // round-trips to query results (post_filter reads documents.metadata_json).
        spc_task.metadata_json = task.metadata_json;
        // ② — the sync call leaves SPCTask.cancelled false; post-parse mid-flight
        // cancel is Phase 2 (cancel's main checkpoint is the per-page parse above).
        int rc = spc_mgr_->ProcessParsedDoc(result, spc_task);
        if (rc != 0) {
            return finalizer_.Fail(task, "CX_ERR_SPC_PROCESS_FAILED", spc_task.error_message,
                                   {{"doc_id", task.doc_id}}, t_start);
        }
    }
    return finalizer_.Complete(task, task.doc_id, t_start);
}

}  // namespace cortrix::async
