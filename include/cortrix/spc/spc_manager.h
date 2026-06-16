#pragma once
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>
#include <atomic>
#include <functional>
#include "cortrix/spc/spc_task.h"
#include "cortrix/spc/spc_task_queue.h"
#include "cortrix/config/config.h"
#include "cortrix/common/status.h"
#include "cortrix/spc/spc_pipeline.h"

namespace cortrix {

namespace resource { class INamespacePool; }  // D3.5 wire⑤: SPC acquires per-task façades from the pool
namespace retrieval { class SparseIndexRegistry; }  // Q4 F40 per-NS SPLADE index owner

/// Manages SPC worker lifecycle and namespace write locks
class SPCManager {
public:
    SPCManager(const SPCConfig& config,
               resource::INamespacePool& pool,
               std::unique_ptr<SPCPipeline> pipeline);
    virtual ~SPCManager();

    /// Submit a task to the processing queue
    /// @return Ok / Unavailable(queue full)
    virtual Status Submit(std::shared_ptr<SPCTask> task);

    /// [Plan B · F42 §4.1.2] Post-parse synchronous entry for the F42 async path.
    /// Acquires the task's façade (like WorkerLoop, minus the blob→temp extraction since
    /// the doc is already parsed) and runs SPCPipeline::ProcessParsed over `parsed`. Used
    /// by DocumentProcessor after its F06 parse (with per-page progress/cancel) to reach
    /// the shared SPC stages (Chunk→META→enrich→embed→write) without re-parsing.
    /// @return 0 = success, -1 = error (task.error_message filled)
    virtual int ProcessParsedDoc(spc::ParsedDoc& parsed, SPCTask& task);

    /// [⑤c via Plan B] Forward the F41 doc-summary enqueue seam to the owned pipeline.
    /// Installed AFTER the WorkerPool exists (the seam captures it); the pipeline has
    /// already moved into this manager — this proxy breaks the
    /// doc_processor → spc_mgr → pipeline(seam) → worker_pool → doc_processor cycle.
    virtual void SetDocSummaryEnqueue(std::function<void(const async::SubmitRequest&)> fn);

    /// [I1 · GS-2] Install the enricher chain (F03 → F35 → F38) on the managed
    /// pipeline. The chain (+ its enrichers) is owned by the caller (bootstrap) and
    /// must outlive this manager; nullptr = single-enricher path. Proxy because the
    /// pipeline has already moved into this manager.
    virtual void SetEnricherChain(cortrix::spc::EnricherChain* chain);

    /// [Q4 · F40] Install the per-NS SPLADE inverted-index registry on the managed
    /// pipeline (proxy, same reason as SetEnricherChain). Owned by the caller
    /// (bootstrap) and shared with the query read path; nullptr = sparse off.
    virtual void SetSparseIndexRegistry(cortrix::retrieval::SparseIndexRegistry* registry);

    /// [D3.5 wire · gap②] Disk write-reject probe (F24 §6, F24-4 decision A). When
    /// set and returning true (disk usage >= CRIT), Submit() refuses NEW task
    /// admission with Unavailable("CX_ERR_DISK_FULL ..."). This is the catch-all
    /// for the non-HTTP ingest paths (DirectoryImporter watcher, Ent CDC) — the
    /// HTTP upload route rejects earlier, before doc/blob are written. main.cpp
    /// binds it to DiskMonitor::ShouldRejectWrites; unset (default) = no gating.
    virtual void SetWriteRejectProbe(std::function<bool()> probe);

    /// Cancel all tasks for a given source_path
    virtual int CancelBySourcePath(const std::string& source_path);

    /// Start worker threads
    virtual void Start();

    /// Stop all workers gracefully
    virtual void Stop();

    /// [D3.5 gap⑤ · F24 §7.2.bis] Graceful-shutdown drain: Stop() (join workers;
    /// each finishes at most its in-hand task) then move out every not-yet-started
    /// queued task. The F24 GracefulShutdown drain hook maps these to PendingTask
    /// for .pending_tasks.json; on the next start ResumeOnStartup re-submits them.
    virtual std::vector<std::shared_ptr<SPCTask>> DrainRemainingTasks();

    /// Get current queue size
    virtual size_t QueueSize() const;

    /// Get task status by task_id
    virtual SPCStage GetTaskStage(int64_t task_id) const;

protected:
    // Protected default constructor for mock subclass
    SPCManager() = default;

private:
    void WorkerLoop();

    SPCConfig config_;
    resource::INamespacePool* pool_ = nullptr;
    std::unique_ptr<SPCPipeline> pipeline_;
    SPCTaskQueue queue_;
    std::vector<std::thread> workers_;
    std::atomic<bool> running_{false};
    std::atomic<int64_t> next_task_id_{1};
    std::function<bool()> write_reject_probe_;  // [gap②] F24 disk gate; null = off
};

}  // namespace cortrix
