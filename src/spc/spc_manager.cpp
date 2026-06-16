#include <cstdint>
#include "cortrix/spc/spc_manager.h"
#include "cortrix/spc/spc_pipeline.h"
#include "cortrix/resource/namespace_facade.h"  // D3.5 wire⑤: per-task NamespaceFacade
#include <chrono>
#include <fstream>
#include <cstdio>

namespace cortrix {

SPCManager::SPCManager(const SPCConfig& config,
                       resource::INamespacePool& pool,
                       std::unique_ptr<SPCPipeline> pipeline)
    : config_(config),
      pool_(&pool),
      pipeline_(std::move(pipeline)),
      queue_(config.max_queue_size) {}

SPCManager::~SPCManager() {
    Stop();
}

Status SPCManager::Submit(std::shared_ptr<SPCTask> task) {
    if (!running_.load()) {
        return Status::Unavailable("SPCManager not running");
    }

    // [D3.5 wire · gap②] Disk pressure gate (F24 §6, F24-4 decision A): at CRIT
    // refuse NEW task admission. Catch-all for the non-HTTP ingest paths (watcher /
    // CDC submit directly here); the HTTP upload route already rejected earlier with
    // the full Agent-friendly 507 body, before doc/blob were written.
    if (write_reject_probe_ && write_reject_probe_()) {
        return Status::Unavailable(
            "CX_ERR_DISK_FULL: disk usage critical, new writes rejected");
    }

    // Assign task ID
    task->task_id = next_task_id_.fetch_add(1);
    task->created_at_us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    int rc = queue_.Push(std::move(task));
    if (rc != 0) {
        return Status::Unavailable("SPC task queue full");
    }
    return Status::Ok();
}

int SPCManager::CancelBySourcePath(const std::string& source_path) {
    return queue_.CancelBySourcePath(source_path);
}

int SPCManager::ProcessParsedDoc(spc::ParsedDoc& parsed, SPCTask& task) {
    // Per-task façade over the F05 pool, mirroring WorkerLoop — but the F42 async path has
    // already parsed (F06 in DocumentProcessor), so no blob→temp extraction is needed here.
    resource::NamespaceFacade facade(*pool_, task.namespace_name);
    Status acq = facade.Acquire();
    if (!acq.ok()) {
        task.stage = SPCStage::kError;
        task.error_message =
            "namespace acquire failed: " + task.namespace_name + ": " + acq.message();
        return -1;
    }
    // The F42 async path (DocumentProcessor → here) carries a doc_id that has NO
    // documents row yet — unlike the upload path, where upload_handler creates the doc
    // before enqueuing. The post-parse stages (F25 write) insert blocks that
    // FK→documents(doc_id), so the row must exist first or block_insert fails with
    // CX_ERR_SPC_PROCESS_FAILED. Create it from the task (doc_get guard keeps this
    // idempotent / avoids a duplicate-PK INSERT), mirroring upload_handler.
    CortrixDoc existing;
    if (facade.store().doc_get(task.doc_id, existing) != 0) {
        CortrixDoc doc;
        doc.doc_id = task.doc_id;
        doc.source_type = task.source_type.empty() ? "file" : task.source_type;
        doc.source_path = task.source_path;
        doc.content_hash = task.content_hash;
        doc.mime_type = task.mime_type;
        doc.processing_level = task.processing_level;
        doc.status = DocStatus::kProcessing;
        if (facade.store().doc_create(doc) != 0) {
            task.stage = SPCStage::kError;
            task.error_message = "doc_create failed for doc_id=" + task.doc_id;
            facade.store().doc_update_status(task.doc_id, DocStatus::kError,
                                             task.error_message);
            return -1;
        }
    } else {
        facade.store().doc_update_status(task.doc_id, DocStatus::kProcessing);
    }
    int rc = pipeline_->ProcessParsed(parsed, task, facade);
    if (rc != 0 && task.stage == SPCStage::kError) {
        facade.store().doc_update_status(task.doc_id, DocStatus::kError, task.error_message);
    }
    return rc;
}

void SPCManager::SetDocSummaryEnqueue(
    std::function<void(const async::SubmitRequest&)> fn) {
    if (pipeline_) pipeline_->SetDocSummaryEnqueue(std::move(fn));
}

void SPCManager::SetEnricherChain(cortrix::spc::EnricherChain* chain) {
    if (pipeline_) pipeline_->SetEnricherChain(chain);
}

void SPCManager::SetSparseIndexRegistry(
    cortrix::retrieval::SparseIndexRegistry* registry) {
    if (pipeline_) pipeline_->SetSparseIndexRegistry(registry);
}

void SPCManager::SetWriteRejectProbe(std::function<bool()> probe) {
    write_reject_probe_ = std::move(probe);
}

void SPCManager::Start() {
    if (running_.exchange(true)) return;  // Already running

    int worker_count = std::max(1, config_.worker_count);
    for (int i = 0; i < worker_count; ++i) {
        workers_.emplace_back(&SPCManager::WorkerLoop, this);
    }
}

void SPCManager::Stop() {
    if (!running_.exchange(false)) return;  // Already stopped

    queue_.Stop();
    for (auto& w : workers_) {
        if (w.joinable()) w.join();
    }
    workers_.clear();
}

std::vector<std::shared_ptr<SPCTask>> SPCManager::DrainRemainingTasks() {
    Stop();  // idempotent; joins workers so no concurrent Pop touches the queue
    return queue_.DrainRemaining();
}

size_t SPCManager::QueueSize() const {
    return queue_.Size();
}

SPCStage SPCManager::GetTaskStage(int64_t task_id) const {
    (void)task_id;
    // MVP: No task tracking map. Return kQueued as default.
    // In Phase 2, maintain a map of task_id -> SPCTask* for status lookup.
    return SPCStage::kQueued;
}

void SPCManager::WorkerLoop() {
    while (running_.load()) {
        auto task = queue_.Pop();
        if (!task) {
            if (!running_.load()) break;
            continue;
        }

        if (task->cancelled.load()) continue;

        // Per-task façade over the F05 pool (D-I5a per-request Acquire/Release):
        // the destructor releases the bundle when this iteration ends.
        resource::NamespaceFacade facade(*pool_, task->namespace_name);
        Status acq = facade.Acquire();
        if (!acq.ok()) {
            task->stage = SPCStage::kError;
            task->error_message =
                "namespace acquire failed: " + task->namespace_name + ": " + acq.message();
            continue;
        }

        // Update doc status to processing
        facade.store().doc_update_status(task->doc_id, DocStatus::kProcessing);

        // For HTTP uploads, extract blob to temp file so the parser can read it
        std::string original_source_path = task->source_path;
        std::string temp_path;
        if (task->source_type == "http_upload") {
            std::vector<uint8_t> blob_data;
            int brc = facade.blob().load(task->namespace_name, task->doc_id, blob_data);
            if (brc != 0 || blob_data.empty()) {
                task->stage = SPCStage::kError;
                task->error_message = "blob load failed for doc_id=" + task->doc_id;
                facade.store().doc_update_status(task->doc_id, DocStatus::kError,
                                                 task->error_message);
                continue;
            }
            temp_path = "/tmp/cortrix_spc_" + task->doc_id + "_" + task->source_path;
            std::ofstream ofs(temp_path, std::ios::binary);
            if (!ofs.is_open()) {
                task->stage = SPCStage::kError;
                task->error_message = "cannot create temp file: " + temp_path;
                facade.store().doc_update_status(task->doc_id, DocStatus::kError,
                                                 task->error_message);
                continue;
            }
            ofs.write(reinterpret_cast<const char*>(blob_data.data()), blob_data.size());
            ofs.close();
            task->source_path = temp_path;
        }

        // Execute pipeline
        int rc = pipeline_->Process(*task, facade);

        // Clean up temp file
        if (!temp_path.empty()) {
            std::remove(temp_path.c_str());
            task->source_path = original_source_path;
        }

        if (rc != 0 && task->stage == SPCStage::kError) {
            // Update doc status to error
            facade.store().doc_update_status(task->doc_id, DocStatus::kError,
                                             task->error_message);
        }
    }
}

}  // namespace cortrix
