#pragma once
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "cortrix/common/status.h"
#include "cortrix/store/i_vector_store.h"
#include "cortrix/store/iindex.h"
#include "cortrix/store/phnsw_config.h"

// hnswlib graph types are referenced only by pointer in this header (the actual
// include is pulled in by phnsw.cpp), so forward declarations keep the public
// header light. Mirrors the MVP CortrixVectorHnswlib forward-decl style.
namespace hnswlib {
template <typename T>
class HierarchicalNSW;
class L2Space;
}  // namespace hnswlib

namespace cortrix::store {

// F01-internal components (defined in src/store/phnsw/). Forward-declared here
// and held by unique_ptr; their headers are included by phnsw.cpp (where the
// out-of-line destructor instantiates them as complete types). WalWriter (S2)
// owns hnsw.wal and is the IWalSink; GroupCommitWriter (shared scaffolding)
// coalesces concurrent writes into one fsync per batch (S3).
class WalWriter;
class GroupCommitWriter;
class SnapshotManager;  // snapshot save/load + recovery (S4)
struct WalEntry;  // codec record applied to the graph during a flush (S3)

/// WAL-specific stats, exposed by PHnsw but not part of the generic IIndex
/// surface (`PhnswWalStats GetWalStats()`). Sourced from the WalWriter.
struct PhnswWalStats {
    uint64_t entry_count = 0;       ///< entries currently in the WAL
    uint64_t committed_lsn = 0;     ///< last durably synced LSN
    std::size_t file_size_bytes = 0;
};

/// Persistent HNSW vector index — a shallow fork of hnswlib that adds a
/// WAL + snapshot persistence layer around the untouched graph algorithm.
///
/// Phase 1 is the only IIndex implementation. PHnsw also implements the narrower
/// IVectorStore (held by WriteCoordinator) — the two bases share most of the
/// surface; where signatures differ (Search ef_search, Exists/Snapshot/Recover
/// TraceContext arity) each override is declared explicitly below.
///
/// Thread safety: writes take a unique_lock on mutex_, reads take a shared_lock
/// (S5). Lifecycle: the constructor runs Recover() and only accepts traffic once
/// the index reports READY.
///
/// S1 scope: this class is declared and instantiable; the WAL / group-commit /
/// snapshot bodies are filled in by S2–S6. Until then the method bodies are
/// minimal stubs (see phnsw.cpp) so downstream features can link against PHnsw
/// and the IIndexFactory.
class PHnsw : public IIndex, public IVectorStore {
public:
    /// unit_data_dir: per-Unit data directory, e.g.
    /// /data/units/unit_legal_0/ — holds hnsw.wal + snapshots + hnsw.meta.
    PHnsw(const std::string& unit_data_dir, const PhnswConfig& config);
    ~PHnsw() override;

    PHnsw(const PHnsw&) = delete;
    PHnsw& operator=(const PHnsw&) = delete;

    // --- Writes (IIndex; internally take the write lock) ---
    Status AddPoint(const float* vec, uint64_t block_id,
                    const observability::TraceContext* ctx = nullptr) override;
    Status AddPoints(const std::vector<std::pair<const float*, uint64_t>>& points,
                     const observability::TraceContext* ctx = nullptr) override;
    /// Idempotent: a missing block_id returns Ok.
    Status MarkDelete(uint64_t block_id,
                      const observability::TraceContext* ctx = nullptr) override;

    // --- Search (IIndex; internally takes the read lock) ---
    std::vector<std::pair<uint64_t, float>> Search(
        const float* query, int top_k, int ef_search = -1,
        const observability::TraceContext* ctx = nullptr) override;

    // --- IIndex membership / persistence / lifecycle ---
    bool Exists(uint64_t block_id) override;
    Status Snapshot() override;
    Status Recover() override;
    Status Shutdown() override;
    IndexStats GetStats() override;
    std::size_t GetMemoryFootprintBytes() const override;

    // --- IVectorStore subset held by the write coordinator (distinct signatures from IIndex) ---
    // Search without ef_search: delegates to the IIndex Search with the
    // configured default. Exists/Snapshot/Recover carry a TraceContext per the
    // ARCHITECTURE.md SoT signature.
    Status Snapshot(const observability::TraceContext* ctx) override;
    Status Recover(const observability::TraceContext* ctx) override;
    bool Exists(uint64_t block_id, const observability::TraceContext* ctx) override;
    std::vector<std::pair<uint64_t, float>> Search(
        const float* query, int top_k,
        const observability::TraceContext* ctx) override;

    // --- PHnsw-specific (not in IIndex / IVectorStore) ---
    PhnswWalStats GetWalStats();

private:
    /// Durable-then-apply sink behind the GroupCommitWriter (S3). The committer
    /// drives exactly one WriteBatch()+Sync() per coalesced batch: WriteBatch
    /// appends the framed records to hnsw.wal (no fsync) and remembers them;
    /// Sync fdatasyncs the WAL and — only after it is durable — replays that
    /// batch into the in-memory graph under the write lock, so AddPoint returns
    /// (its future resolves) only once the record is both crash-safe and visible
    /// (design § 4.1). Defined in phnsw.cpp.
    class ApplySink;

    // Apply one decoded WAL record to the graph. Caller holds the write lock.
    void ApplyEntryLocked(const WalEntry& entry);

    // Apply an INSERT record. Caller holds the write lock (inside
    // ApplyEntryLocked's try). Resurrects the SAME block_id when it is currently
    // marked deleted (the re-write path: a marked-deleted label would otherwise
    // make hnswlib's addPoint throw under allow_replace_deleted_, silently
    // dropping the durably-logged vector).
    void ApplyInsertLocked(const WalEntry& entry);

    // True iff `label` is present in the graph AND marked deleted (the re-write
    // precondition). Caller holds the write lock; reads hnswlib's own label map.
    bool IsLabelMarkedDeletedLocked(uint64_t label) const;

    // KNN search body; caller holds mutex_ (shared for the default-ef path,
    // exclusive when a per-query ef_search is applied). Pure graph read (S5).
    std::vector<std::pair<uint64_t, float>> SearchLocked(const float* query, int top_k);

    // Memory estimate body; caller holds mutex_ (shared or exclusive). Split out
    // so GetStats() can reuse it under its own lock without a self-deadlocking
    // second shared_lock acquisition (S5).
    std::size_t GetMemoryFootprintBytesLocked() const;

    // S4: load the newest snapshot into index_ then replay hnsw.wal entries with
    // LSN > snapshot_lsn (design § 4.4). Run once from the constructor (and
    // re-runnable via Recover()). Returns the recovery Status; on success the
    // index is ready. Builds a fresh empty index when there is no snapshot.
    Status RecoverInternal();

    // S6 auto-snapshot (design § 4.1 step 6): after a write batch is applied, if
    // the WAL exceeds snapshot_max_wal_entries or snapshot_max_wal_size_mb, wake
    // the background snapshot thread (coalescing — many crossings collapse into
    // one pending request). Caller holds the write lock (called from the flush).
    void MaybeRequestSnapshotLocked();
    // Background thread body: waits for a request, then runs Snapshot() (which
    // takes the write lock itself, so it never runs inside the flush's lock).
    void SnapshotLoop();

    std::unique_ptr<hnswlib::L2Space> space_;
    std::unique_ptr<hnswlib::HierarchicalNSW<float>> index_;
    std::unique_ptr<SnapshotManager> snapshot_mgr_;   // S4: snapshot save/load
    std::unique_ptr<WalWriter> wal_;                  // S2: owns hnsw.wal (= sink)
    std::unique_ptr<ApplySink> apply_sink_;           // S3: WAL-durable -> graph apply
    std::unique_ptr<GroupCommitWriter> committer_;    // S3: batches AddPoint -> one fsync
    mutable std::shared_mutex mutex_;                 // S5: multi-read / single-write
    PhnswConfig config_;
    std::string unit_data_dir_;                       // per-Unit data dir (V5 #10)
    std::size_t deleted_count_ = 0;
    // Global LSN of the last WAL record actually APPLIED to the graph (advances
    // under the write lock in lock-step with ApplyEntryLocked). Snapshot records
    // this — not the WAL's committed_lsn — so a snapshot's lsn always matches the
    // graph state it captured, even if a record is durably written but not yet
    // applied (design § 4.4 recovery filtering).
    uint64_t applied_lsn_ = 0;
    bool ready_ = false;                              // §4.4-ter READY gate

    // S6 auto-snapshot background thread + its coalescing request flag. The
    // thread runs Snapshot() off the write path so a large flush never blocks on
    // serialization; snapshot_requested_ collapses repeated threshold crossings
    // into one pending snapshot. Started after recovery, stopped in the dtor.
    std::thread snapshot_thread_;
    std::mutex snapshot_mu_;
    std::condition_variable snapshot_cv_;
    bool snapshot_requested_ = false;                // guarded by snapshot_mu_
    bool snapshot_stop_ = false;                     // guarded by snapshot_mu_
};

}  // namespace cortrix::store
