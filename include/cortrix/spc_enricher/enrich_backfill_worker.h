#pragma once
#include <cstdint>
#include <memory>
#include <string>

#include "cortrix/async/task_finalizer.h"
#include "cortrix/async/task_handler.h"
#include "cortrix/async/task_info.h"
#include "cortrix/common/status.h"

namespace cortrix {

class OnnxEmbedder;
namespace resource { class INamespacePool; }

namespace spc {

class EnricherChain;

/// EnrichBackfillWorker — the F42 async-task callback for kTaskEnrichBackfill
/// (addendum §3.7 Fork-1 B). For one document it repairs the chunk-level
/// enrichment debt recorded in enrich_state:
///   (1) acquires the task's NamespaceFacade (RAII Release, F41 pattern);
///   (2) loads the doc's 'pending_retry' enrich_state rows; nothing pending →
///       Complete (idempotent — sweeper double-enqueue / crash replay safe);
///   (3) rebuilds ChunkContexts from the persisted blocks/parents/documents rows
///       (text + prev/next neighbors from the full child order + doc metadata);
///   (4) groups rows by their owed failed_members csv and re-runs the production
///       EnricherChain with the matching member_filter — repaired members are
///       never re-run (duplicate-artifact guard), still-owed members are;
///   (5) writes the repaired artifacts with ingest write-parity, in one F25
///       WriteCoordinator txn when blocks/vectors are added: F03 enrichment +
///       entities (+ summary into blocks.metadata_json), F35 contextualized
///       columns + dual-vector point + label row, F38 hype blocks + points.
///       Each artifact write is guarded by an already-present check so a crash
///       between commit and the state flip cannot double-write;
///   (6) flips enrich_state: fully repaired → 'ok'; still owed → attempts+1 and
///       next_retry_at += NextRetryDelaySec(attempts), or 'failed_permanent'
///       past kMaxAttempts.
///
/// The F07 semantic_score is NOT recomputed for repaired chunks (the write-time
/// score keeps its no-enricher value): the parser identity needed by the scorer
/// is not persisted per block, and the relative order among backfilled chunks is
/// unaffected. Recorded as a known deviation in addendum §3.7.4.
class EnrichBackfillWorker : public async::ITaskHandler {
public:
    /// @param pool     borrowed F05 namespace pool (per-task Acquire/Release).
    /// @param chain    borrowed production enricher chain (nullptr / no member
    ///                 available → the task fails transiently; rows stay pending).
    /// @param embedder borrowed OnnxEmbedder (F38 hype-question embeddings; F35
    ///                 embeds internally through its own adapter).
    /// @param task_mgr borrowed TaskManager for the TaskFinalizer terminal write.
    EnrichBackfillWorker(resource::INamespacePool& pool, EnricherChain* chain,
                         OnnxEmbedder& embedder, async::TaskManager* task_mgr);

    Status ProcessTask(const async::TaskInfo& task) override;

    /// Exponential backoff schedule (addendum §3.7): 60s → 300 → 900 → 3600 →
    /// 21600, capped at 21600 (6h). `attempts` = attempts already spent.
    static int64_t NextRetryDelaySec(int attempts);

    /// Give-up threshold → 'failed_permanent' (addendum §3.7 default).
    static constexpr int kMaxAttempts = 8;

private:
    resource::INamespacePool& pool_;
    EnricherChain* chain_;
    OnnxEmbedder& embedder_;
    async::TaskFinalizer finalizer_;
};

}  // namespace spc
}  // namespace cortrix
