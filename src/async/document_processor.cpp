#include "cortrix/async/document_processor.h"

#include <algorithm>
#include <chrono>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "cortrix/async/f42_error.h"
#include "cortrix/spc/parser.h"
#include "cortrix/spc/parser_errors.h"
#include "cortrix/spc/spc_manager.h"
#include "cortrix/spc/spc_task.h"

namespace cortrix::async {

namespace {
// [Plan B · F42 §4.1.2 ①] Async TaskInfo lacks mime_type; derive just enough from the
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
                                     cortrix::SPCManager* spc_mgr)
    : mgr_(mgr),
      factory_(factory),
      config_(config),
      cancel_checker_(std::move(cancel_checker)),
      finalizer_(mgr),
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
        auto r = config_->GetInt("f42.async_max_pages");
        if (r.ok()) return r.value();
    }
    return kDefaultAsyncMaxPages;
}

Status DocumentProcessor::ProcessTask(const TaskInfo& task) {
    // F42 §6.bis metrics — record the terminal outcome + end-to-end duration,
    // keyed on the task's task_type. The /metrics endpoint wiring is D3.5; the
    // recorder + these feed points are the D3 piece.
    const auto t_start = std::chrono::steady_clock::now();

    // Local progress accumulator mirrored into the tasks table each page.
    TaskInfo prog = task;
    prog.current_phase = task_phase::kParsing;  // topic 5 — only the parse phase is wired in D3
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
        // ETA: remaining pages × observed per-page time is wired in S3/D3.5 with a
        // real clock; D3 records -1 (unknown) until the streaming driver exists.
        prog.eta_seconds = -1;
        Status s = mgr_->UpdateProgress(prog);
        if (!s.ok()) {
            spdlog::warn("F42 progress persist failed for {}: {}", prog.task_id,
                         s.message());
        }
    };

    spc::ParsedDoc result;
    try {
        result = factory_->ParseDocument(task.filepath, opts);
    } catch (const CancellationException&) {
        // topic 3.3 A — F06 subprocess is not force-killed; the current page
        // finished naturally and the checkpoint threw. Finalize as cancelled.
        // DEFERRED → D3.5: if a downstream F25 BeginWrite had already landed
        // chunks, invoke RollbackCallback here (nothing is written in D3, so the
        // cancel is clean).
        return finalizer_.Cancel(task, t_start);
    }

    if (!result.ok()) {
        // Map the F06 ParserErrorCode → CX_ERR_PARSE_FAILED + GEN-Agent structured_data
        // (topic 5 error persistence); the finalizer persists via MarkFailed + metric.
        nlohmann::json sd = {
            {"task_id", task.task_id},
            {"page_number", result.failed_pages.empty() ? -1 : result.failed_pages.front()},
            {"parser_error", result.error_msg},
        };
        return finalizer_.Fail(task, F42ErrorCodeString(F42ErrorCode::kParseFailed),
                               result.error_msg, sd, t_start);
    }

    // Re-check cancel after parsing, before declaring completion: a cancel that
    // arrived on the last page (so the checkpoint didn't fire again) still wins.
    if (cancel_checker_ && cancel_checker_(task.task_id)) {
        return finalizer_.Cancel(task, t_start);
    }

    // [Plan B · F42 §4.1.2] If wired to SPC, hand the parsed doc to the shared post-parse
    // stages (Chunk→F08 META→F03 enrich→embed→assemble→F25 write) via SPCManager; else
    // (standalone D3) finalize parse-only with the task's content-hash-derived doc_id.
    if (spc_mgr_) {
        SPCTask spc_task;
        spc_task.doc_id = task.doc_id;
        spc_task.namespace_name = task.namespace_id;
        spc_task.source_path = task.filepath;
        spc_task.content_hash = task.content_hash;
        spc_task.processing_level = 3;  // L3 full processing for async large docs
        spc_task.source_type = "file";
        spc_task.mime_type = InferMimeFromFilename(task.filename);
        // §4.1.2 ② — the sync call leaves SPCTask.cancelled false; post-parse mid-flight
        // cancel is Phase 2 (cancel's main checkpoint is the F06 per-page parse above).
        int rc = spc_mgr_->ProcessParsedDoc(result, spc_task);
        if (rc != 0) {
            return finalizer_.Fail(task, "CX_ERR_SPC_PROCESS_FAILED", spc_task.error_message,
                                   {{"doc_id", task.doc_id}}, t_start);
        }
    }
    return finalizer_.Complete(task, task.doc_id, t_start);
}

}  // namespace cortrix::async
