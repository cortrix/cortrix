#include <cstdint>
#include "cortrix/catalog/gc/document_gc_sweeper.h"

#include <chrono>
#include <limits>
#include <optional>
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

namespace {

DocumentGcSweeper::RestoreDecision Refused(const std::string& id, const char* code,
                                           std::string message,
                                           std::string live_doc_id = "",
                                           std::string ns_id = "") {
    DocumentGcSweeper::RestoreDecision d;
    d.kind = DocumentGcSweeper::RestoreDecision::Kind::kRefused;
    d.failure = {id, code, std::move(message), std::move(live_doc_id), std::move(ns_id)};
    return d;
}

DocumentGcSweeper::RestoreDecision CheckFailed(const std::string& id) {
    return Refused(id, "CX_ERR_GC_RESTORE_CHECK_FAILED",
                   "could not check this document's versions; retry");
}

}  // namespace

DocumentGcSweeper::RestoreDecision DocumentGcSweeper::DecideRestore(
    CortrixStore& store, const std::string& doc_id, const std::string& ns_id) {
    CortrixDoc row;
    const int got = store.doc_get(doc_id, row);
    if (got == -2 || (got == 0 && row.status != DocStatus::kDeleted)) return {};
    if (got != 0) return CheckFailed(doc_id);

    // 1. A version that never finished processing comes back with partial or no
    //    content, so it is refused. A memory session is exempt: its status describes
    //    only its last turn, and the earlier turns' blocks are intact. A row deleted
    //    before its store recorded the status reads as unknown and is not refused here.
    if (row.source_type != "memory_session") {
        std::optional<std::string> deleted_from;
        const int mrc = store.doc_get_deleted_from_status(doc_id, &deleted_from);
        if (mrc == -2) return {};
        if (mrc != 0) return CheckFailed(doc_id);
        if (deleted_from.has_value() && *deleted_from != "ready") {
            return Refused(doc_id, "CX_ERR_GC_RESTORE_INCOMPLETE",
                           "this version never finished processing; add the document again",
                           "", ns_id);
        }
    }

    // 2. For the two producers whose repeated source is a new version, a restore never
    //    leaves two live versions of one source: any live row of the source refuses it.
    //    The check and the restore are one statement, so a version that finishes
    //    meanwhile either refuses the restore or retires the restored row.
    if (row.source_type == "http_upload" || row.source_type == "watch_dir") {
        const int rrc = store.doc_restore_unless_superseded(doc_id);
        if (rrc == 0) {
            RestoreDecision d;
            d.kind = RestoreDecision::Kind::kRestored;
            return d;
        }
        if (rrc != -2) return CheckFailed(doc_id);
        // Nothing changed. Tell a row that is gone, or live again, from one that a live
        // version of its source keeps deleted. A row still deleted with no live version
        // lost it meanwhile, or a read failed: a retry decides.
        CortrixDoc current;
        const int again = store.doc_get(doc_id, current);
        if (again == -2 || (again == 0 && current.status != DocStatus::kDeleted)) return {};
        if (again != 0) return CheckFailed(doc_id);
        CortrixDoc live;
        if (store.doc_find_by_source(current.source_type, current.source_path, live) == 0) {
            return Refused(doc_id, "CX_ERR_GC_RESTORE_SUPERSEDED",
                           "another version is live: delete " + live.doc_id +
                               " in namespace " + ns_id + ", then restore " + doc_id,
                           live.doc_id, ns_id);
        }
        return CheckFailed(doc_id);
    }

    // 3. Restore.
    const int rrc = store.doc_restore(doc_id);
    if (rrc == 0) {
        RestoreDecision d;
        d.kind = RestoreDecision::Kind::kRestored;
        return d;
    }
    if (rrc == -2) return {};
    return CheckFailed(doc_id);
}

Result<DocumentGcSweeper::RestoreOutcome> DocumentGcSweeper::Restore(
    const std::vector<std::string>& doc_ids) {
    std::lock_guard<std::mutex> lock(mu_);
    RestoreOutcome out;

    auto ns_list = ns_router_->ListNamespaces();
    if (!ns_list.ok()) return ns_list.status();

    for (const auto& doc_id : doc_ids) {
        bool decided = false;
        // doc_id carries no NS, so search namespaces for a soft-deleted match; the
        // first one that holds it decides, as a refusal ends the search too.
        for (const auto& ns_id : ns_list.value().results) {
            resource::NamespaceFacade facade(*pool_, ns_id);
            if (!facade.Acquire().ok()) continue;
            const RestoreDecision d = DecideRestore(facade.store(), doc_id, ns_id);
            if (d.kind == RestoreDecision::Kind::kNotHere) continue;
            if (d.kind == RestoreDecision::Kind::kRestored) {
                out.succeeded.push_back(doc_id);
            } else {
                out.failed.push_back(d.failure);
            }
            decided = true;
            break;
        }
        if (!decided) {
            out.failed.push_back({doc_id, "CX_ERR_GC_DOC_NOT_FOUND",
                                  "no deleted document with this id", "", ""});
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
