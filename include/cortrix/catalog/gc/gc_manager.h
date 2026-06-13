#pragma once
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "cortrix/catalog/gc/blob_gc_sink.h"
#include "cortrix/common/result.h"
#include "cortrix/common/status.h"
#include "cortrix/config/config.h"

typedef struct sqlite3 sqlite3;

namespace cortrix::catalog::gc {

/// Snapshot of GC state for `GET /gc/status` (ARCH GcStatus / schemas.yaml).
/// Field set is the authoritative GcStatus contract consumed by the P03 SDK
/// dataclass (status / soft_deleted_count / reclaimable_bytes / last_gc_at).
struct GcStatusSnapshot {
    bool running = false;                  ///< true while a sweep is in progress
    int64_t soft_deleted_count = 0;        ///< file_locations rows in Stage 1 (status='deleted')
    int64_t reclaimable_bytes = 0;         ///< size_bytes summed over GC-eligible blobs
    std::optional<int64_t> last_gc_at_ms;  ///< wall-clock ms of the last completed sweep (nullopt if never)
};

/// Outcome of one three-stage sweep (RunOnce) — also returned by Run/Purge so the
/// route layer can report what changed and rebuild a fresh GcStatusSnapshot.
struct GcRunReport {
    int hard_deleted = 0;     ///< Stage 2: file_locations rows hard-deleted + enqueued
    int blobs_unlinked = 0;   ///< Stage 3: blob_gc_queue rows physically unlinked
    bool hit_run_cap = false; ///< true if max_purge_per_run / max_run_duration stopped the sweep early
};

/// OPEN-2 three-stage GC over catalog.db (ARCH §5.x). Drives the soft-delete →
/// hard-delete → blob-unlink state machine; `GcThread` wraps it in a background
/// scan loop. Borrows the catalog sqlite3 handle (owned by CatalogDb), an
/// IContentRefStore for ref_count fix-up, and an IBlobGcSink for Stage 3 unlinks.
///
///   Stage 1 (soft delete): set elsewhere (file_locations.status='deleted',
///            deleted_at=NOW); restorable for soft_delete_retention_days.
///   Stage 2 (hard delete): rows whose deleted_at is older than the retention
///            window have ref_count recomputed (content_refs active rows); when
///            it reaches 0 the row is removed and its blob_uri enqueued into
///            blob_gc_queue with eligible_at = NOW + blob_gc_retention_days.
///   Stage 3 (blob GC): blob_gc_queue rows past eligible_at are unlinked via the
///            sink and marked 'unlinked'.
///
/// Thread-safe: a mutex serializes sweeps so the background thread and a manual
/// /gc/run never overlap; Status() takes the same lock for a consistent snapshot.
class GcManager {
public:
    GcManager(sqlite3* db, IBlobGcSink* sink, const GcConfig& config);

    GcManager(const GcManager&) = delete;
    GcManager& operator=(const GcManager&) = delete;

    /// Run one full three-stage sweep (Stage 2 then Stage 3), bounded by
    /// max_purge_per_run / max_run_duration_minutes. `dry_run` scans + reports
    /// counts without deleting. Serialized against concurrent sweeps.
    Result<GcRunReport> RunOnce();

    /// Current GC state snapshot (ARCH GcStatus shape). Reflects an in-progress
    /// sweep via `running`.
    Result<GcStatusSnapshot> GetStatus() const;

    /// Restore soft-deleted documents (Stage 1 reversal): clear status/deleted_at
    /// on the file_locations rows referenced by these doc_ids and reactivate their
    /// content_refs. Returns per-id success/failure for PartialSuccessById.
    struct RestoreOutcome {
        std::vector<std::string> succeeded;  ///< doc_ids restored
        std::vector<std::pair<std::string, std::string>> failed;  ///< (doc_id, CX_ERR_* reason)
    };
    Result<RestoreOutcome> Restore(const std::vector<std::string>& doc_ids);

    /// Immediate purge (ops /gc/purge): bypass the time windows and run Stage 2 +
    /// Stage 3 on every currently soft-deleted item. Honours dry_run + the per-run
    /// caps. Distinct from the GDPR immediate-purge API (Cloud V1).
    Result<GcRunReport> Purge();

    /// Stage 2 hook used by delete paths: enqueue a blob_uri for GC with
    /// eligible_at = NOW + blob_gc_retention_days. Idempotent on blob_uri.
    Status EnqueueBlob(const std::string& blob_uri, const std::string& file_hash);

    /// Maintenance op (/maintenance/vacuum): SQLite VACUUM over catalog.db to
    /// reclaim free pages + compact. Serialized against sweeps.
    Status Vacuum();

    /// Maintenance op (/maintenance/reindex): SQLite REINDEX over catalog.db to
    /// rebuild indexes (repairs a corrupted/ bloated index). Serialized.
    Status Reindex();

    const GcConfig& config() const { return config_; }

private:
    // Sweep helpers — all run with mu_ held.
    Result<GcRunReport> RunLocked(bool bypass_windows);
    Status Stage2HardDelete(int64_t now_ms, bool bypass_windows, int* out_count,
                            int cap, int64_t deadline_ms, bool* hit_cap);
    Status Stage3BlobGc(int64_t now_ms, bool bypass_windows, int* out_count,
                        int cap, int64_t deadline_ms, bool* hit_cap);

    sqlite3* db_;                  // borrowed, not owned
    IBlobGcSink* sink_;            // borrowed
    GcConfig config_;

    mutable std::mutex mu_;
    bool running_ = false;
    std::optional<int64_t> last_gc_at_ms_;
};

}  // namespace cortrix::catalog::gc
