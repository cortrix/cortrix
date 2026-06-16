#include <cstdint>
#include "cortrix/catalog/gc/document_gc_sweeper.h"

#include <chrono>
#include <limits>
#include <string>
#include <vector>

#include "cortrix/catalog/i_ns_router.h"
#include "cortrix/common/data_types.h"
#include "cortrix/resource/namespace_facade.h"
#include "cortrix/store/cortrix_blob_store.h"
#include "cortrix/store/cortrix_store.h"
#include "cortrix/store/iindex.h"

namespace cortrix::catalog::gc {

namespace {

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

int64_t DaysToMs(int days) { return static_cast<int64_t>(days) * 24 * 3600 * 1000; }

}  // namespace

DocumentGcSweeper::DocumentGcSweeper(catalog::INSRouter* ns_router,
                                     resource::INamespacePool* pool,
                                     const GcConfig& config)
    : ns_router_(ns_router), pool_(pool), config_(config) {}

Result<DocumentGcSweeper::SweepReport> DocumentGcSweeper::RunOnce() {
    std::lock_guard<std::mutex> lock(mu_);
    return RunLocked(/*bypass_windows=*/false);
}

Result<DocumentGcSweeper::SweepReport> DocumentGcSweeper::Purge() {
    std::lock_guard<std::mutex> lock(mu_);
    return RunLocked(/*bypass_windows=*/true);
}

Result<DocumentGcSweeper::SweepReport> DocumentGcSweeper::RunLocked(bool bypass_windows) {
    SweepReport report;
    const int64_t now = NowMs();
    // cutoff = now - retention window; bypass collapses the window so every
    // soft-deleted doc qualifies (use max int64 so deleted_at <= cutoff is always true).
    const int64_t cutoff = bypass_windows
                               ? std::numeric_limits<int64_t>::max()
                               : now - DaysToMs(config_.soft_delete_retention_days);
    const int cap = config_.max_purge_per_run;

    auto ns_list = ns_router_->ListNamespaces();
    if (!ns_list.ok()) return ns_list.status();

    for (const auto& ns_id : ns_list.value().results) {
        if (report.docs_hard_deleted >= cap) {
            report.hit_run_cap = true;
            break;
        }
        resource::NamespaceFacade facade(*pool_, ns_id);
        if (!facade.Acquire().ok()) continue;  // NS gone / unloadable → skip

        std::vector<CortrixDoc> expired;
        if (facade.store().doc_list_deleted_before(cutoff, expired) != 0) continue;

        for (const auto& doc : expired) {
            if (report.docs_hard_deleted >= cap) {
                report.hit_run_cap = true;
                break;
            }
            if (config_.dry_run) {
                ++report.docs_hard_deleted;
                continue;
            }

            // 1) Tombstone each block's vector (idempotent). The blocks still
            //    carry their hnsw block_id; MarkDelete removes them from search.
            std::vector<CortrixBlock> blocks;
            facade.store().block_get_by_doc(doc.doc_id, blocks);
            for (const auto& b : blocks) {
                facade.vec_index().MarkDelete(b.block_id);
            }
            // 2) Delete the physical blob (.raw). del() 0/-2 = gone (idempotent).
            facade.blob().del(ns_id, doc.doc_id);
            ++report.blobs_deleted;
            // 3) Hard-delete the block rows + the document row.
            facade.store().block_delete_by_doc(doc.doc_id);
            report.blocks_deleted += static_cast<int>(blocks.size());
            facade.store().doc_delete(doc.doc_id);
            ++report.docs_hard_deleted;
        }
    }
    return report;
}

Result<DocumentGcSweeper::RestoreOutcome> DocumentGcSweeper::Restore(
    const std::vector<std::string>& doc_ids) {
    std::lock_guard<std::mutex> lock(mu_);
    RestoreOutcome out;

    auto ns_list = ns_router_->ListNamespaces();
    if (!ns_list.ok()) return ns_list.status();

    for (const auto& doc_id : doc_ids) {
        bool restored = false;
        // doc_id carries no NS, so search namespaces for a soft-deleted match.
        for (const auto& ns_id : ns_list.value().results) {
            resource::NamespaceFacade facade(*pool_, ns_id);
            if (!facade.Acquire().ok()) continue;
            const int rc = facade.store().doc_restore(doc_id);
            if (rc == 0) {  // restored in this NS
                restored = true;
                break;
            }
            // rc == -2 here means "not in this NS or not soft-deleted"; keep looking.
        }
        if (restored) {
            out.succeeded.push_back(doc_id);
        } else {
            out.failed.emplace_back(doc_id, "CX_ERR_GC_DOC_NOT_FOUND");
        }
    }
    return out;
}

Result<int64_t> DocumentGcSweeper::CountSoftDeleted() const {
    std::lock_guard<std::mutex> lock(mu_);
    auto ns_list = ns_router_->ListNamespaces();
    if (!ns_list.ok()) return ns_list.status();

    int64_t total = 0;
    for (const auto& ns_id : ns_list.value().results) {
        resource::NamespaceFacade facade(*pool_, ns_id);
        if (!facade.Acquire().ok()) continue;
        std::vector<CortrixDoc> deleted;
        // A future-max cutoff selects every soft-deleted doc regardless of age.
        if (facade.store().doc_list_deleted_before(std::numeric_limits<int64_t>::max(),
                                                   deleted) == 0) {
            total += static_cast<int64_t>(deleted.size());
        }
    }
    return total;
}

}  // namespace cortrix::catalog::gc
