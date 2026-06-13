#pragma once
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "cortrix/common/result.h"
#include "cortrix/common/status.h"
#include "cortrix/config/config.h"

namespace cortrix::catalog {
class INSRouter;
}
namespace cortrix::resource {
class INamespacePool;
}

namespace cortrix::catalog::gc {

/// Per-Unit document GC (OPEN-2 Stage 2/3 over the document layer, B① layering).
///
/// GcManager operates on catalog.db (file_locations / blob_gc_queue) and cannot
/// reach the per-Unit stores where live documents, blocks, vectors and blobs
/// live. DocumentGcSweeper fills that gap: it iterates namespaces (via INSRouter),
/// acquires each NamespaceFacade (via INamespacePool), and runs the document
/// lifecycle:
///   - Stage 2 hard delete: soft-deleted docs whose deleted_at is older than
///     gc.soft_delete_retention_days (0 = immediate) → MarkDelete each block's
///     vector, blob().del(ns, doc_id), then block_delete_by_doc + doc_delete.
///   - Restore: flip a soft-deleted doc back to live (within the window).
///   - Purge: immediate hard delete bypassing the retention window.
///
/// The background GcThread calls both GcManager::RunOnce (catalog/blob_uri layer)
/// and DocumentGcSweeper::RunOnce (per-Unit document layer) each cycle; the two
/// own disjoint storage layers.
class DocumentGcSweeper {
public:
    DocumentGcSweeper(catalog::INSRouter* ns_router, resource::INamespacePool* pool,
                      const GcConfig& config);

    DocumentGcSweeper(const DocumentGcSweeper&) = delete;
    DocumentGcSweeper& operator=(const DocumentGcSweeper&) = delete;

    struct SweepReport {
        int docs_hard_deleted = 0;   ///< documents removed (rows + blocks + blob)
        int blocks_deleted = 0;      ///< block rows removed across all docs
        int blobs_deleted = 0;       ///< .raw blob files unlinked
        bool hit_run_cap = false;    ///< max_purge_per_run stopped the sweep early
    };

    /// Stage 2 over every namespace: hard-delete soft-deleted docs past the
    /// retention window. Bounded by gc.max_purge_per_run (docs). dry_run reports
    /// counts without mutating.
    Result<SweepReport> RunOnce();

    /// Immediate purge: like RunOnce but bypasses the retention window (every
    /// soft-deleted doc qualifies). Honors dry_run + the per-run cap.
    Result<SweepReport> Purge();

    /// Restore soft-deleted documents by doc_id (Stage 1 reversal). doc_ids carry
    /// no namespace, so each is searched across namespaces. Returns per-id outcome.
    struct RestoreOutcome {
        std::vector<std::string> succeeded;                       ///< doc_ids restored
        std::vector<std::pair<std::string, std::string>> failed;  ///< (doc_id, CX_ERR_*)
    };
    Result<RestoreOutcome> Restore(const std::vector<std::string>& doc_ids);

    /// Count of currently soft-deleted docs across all namespaces (for GcStatus).
    Result<int64_t> CountSoftDeleted() const;

    const GcConfig& config() const { return config_; }

private:
    // One sweep over every namespace; mu_ held. bypass_windows collapses the
    // retention window (immediate purge).
    Result<SweepReport> RunLocked(bool bypass_windows);

    catalog::INSRouter* ns_router_;        // borrowed
    resource::INamespacePool* pool_;       // borrowed
    GcConfig config_;
    mutable std::mutex mu_;
};

}  // namespace cortrix::catalog::gc
