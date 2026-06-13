#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "cortrix/common/result.h"
#include "cortrix/common/status.h"
#include "cortrix/common/types.h"
#include "cortrix/store/i_vector_store.h"   // F01 IVectorStore (canonical Exists, Q1-C)
#include "cortrix/store/pending_entry.h"
#include "cortrix/store/pending_log_writer.h"
#include "cortrix/store/recover_stores.h"   // IMetadataStore / IBlobStore (F25-consumer DIP)

namespace cortrix::store {

/// Tuning for the Write Coordinator (design § 2.2). Defaults match the spec.
struct WriteCoordinatorConfig {
    // pending.wal group-commit flush policy (shared GroupCommitWriter).
    int group_commit_max_batch = 32;
    int group_commit_timeout_ms = 5;

    // Commit retry on transient pending.wal write failure (Q1-B): the i-th
    // retry waits commit_retry_base_ms * 10^i  → 1ms, 10ms, 100ms by default.
    int commit_max_retries = 3;
    int commit_retry_base_ms = 1;

    // Inline compaction (Q3): compact pending.wal every N committed txns.
    int compact_threshold = 1000;

    // Recovery policy (design § 2.2).
    int max_recovery_entries = 10000;  // abort recovery past this many records
    bool strict_recovery = false;      // true: any cleanup failure aborts startup
};

/// Handle returned by BeginWrite, passed back to Commit / Rollback.
struct TxnHandle {
    uint64_t txn_id = 0;
    uint64_t lsn = 0;  ///< reserved; Phase 1 mirrors txn_id (no per-record LSN)
};

// WalStats (pending/committed/rollback counts) is defined in
// pending_log_writer.h and reused here — the coordinator's GetStats is a thin
// pass-through over the writer's counts (design § 2.1).

/// Write Coordinator — the PWL transaction state machine (design § 2.1).
///
/// A document write is PENDING → COMMITTED/ROLLBACK, persisted via the
/// PendingLogWriter (S1). The caller drives the three-way store write between
/// BeginWrite and Commit; on any store failure it calls Rollback, which appends
/// a ROLLBACK record and invokes the registered RollbackCallback to clean up
/// whatever already landed (callback must be idempotent, Q6).
///
/// In-memory state tables (active_txns_ / active_docs_) give process-local
/// defense against double-Commit, Commit-after-Rollback, Commit-without-Begin
/// and same-doc_id concurrent writes (Q5/Q2) without a disk read on the hot
/// path. They are authoritative only within a process lifetime; crash recovery
/// (S3) rebuilds intent from pending.wal + the three-way consistency check.
///
/// Thread-safe: BeginWrite/Commit/Rollback may be called concurrently. Same
/// doc_id must not be written concurrently by the caller (Q2 is an assertion-
/// level guard, not a business feature — design § 1 boundary).
class WriteCoordinator {
public:
    /// @param data_dir       per-namespace dir; pending.wal lives here
    /// @param config         tuning (see WriteCoordinatorConfig)
    /// @param vector_store   borrowed (F01 IVectorStore); Recover's Exists check
    /// @param metadata_store borrowed; Recover's BlockExists check
    /// @param blob_store     borrowed; Recover's BlobExists check
    /// Any store may be null when Recover() is not exercised (e.g. write-only
    /// unit tests); Recover() requires all three (design § 4.4 Q1-C).
    WriteCoordinator(const std::string& data_dir, const WriteCoordinatorConfig& config,
                     IVectorStore* vector_store, IMetadataStore* metadata_store,
                     IBlobStore* blob_store);
    ~WriteCoordinator();

    WriteCoordinator(const WriteCoordinator&) = delete;
    WriteCoordinator& operator=(const WriteCoordinator&) = delete;

    /// Rollback cleanup hook (design § 2.1). Receives the originating PENDING
    /// entry (doc_id + block_ids) so the layer above can drop whatever it wrote.
    /// Must be idempotent: cleaning data that is absent returns Ok (Q6).
    using RollbackCallback = std::function<Status(const PendingEntry&)>;

    /// Open pending.wal (creates it on first use). Must succeed before any
    /// BeginWrite. Returns CX_ERR_PWL_WRITE_FAILED on I/O error,
    /// CX_ERR_PWL_CORRUPTED on a damaged header.
    Status Init();

    /// Register the rollback cleanup callback. Call before BeginWrite; not
    /// thread-safe against concurrent Rollback.
    void SetRollbackCallback(RollbackCallback cb);

    /// Crash recovery (design § 4.4). Scans pending.wal in EOF order, groups
    /// records by txn, and for each:
    ///   - has COMMITTED → already durable, nothing to do;
    ///   - has ROLLBACK  → replay the idempotent cleanup callback (Q6);
    ///   - PENDING only  → three-way consistency check (Q1-C): if the vector,
    ///     metadata and blob stores all hold the data, infer COMMITTED (the WAL
    ///     record was lost after the stores were written) and keep it; if any
    ///     is missing, the crash happened mid-write → replay cleanup.
    /// Then compacts pending.wal, reseeds next_txn_id_ to max(seen)+1, and clears
    /// the in-memory tables. Call once at startup, after SetRollbackCallback and
    /// before BeginWrite. Requires all three stores to be non-null.
    /// Returns CX_ERR_PWL_RECOVERY_FAILED if record count exceeds
    /// max_recovery_entries, or (strict_recovery=true only) if a cleanup fails.
    Status Recover();

    // --- transaction interface ---

    /// Begin a document write: allocate a txn_id, append PENDING, record intent
    /// in the state tables. `writes_blob` records whether this txn writes a blob
    /// (v1.0.2 D3.5 #1): a blob-less doc (watch_dir/CDC/memory) passes false so
    /// Recover does not read its (legitimately absent) blob as an incomplete write
    /// (§4.4, the §10.4 data-loss fix). Returns CX_ERR_PWL_DOC_WRITE_IN_PROGRESS if
    /// `doc_id` already has a live transaction (Q2), CX_ERR_PWL_WRITE_FAILED on I/O.
    Status BeginWrite(const std::string& doc_id, const std::vector<BlockId>& block_ids,
                      bool writes_blob, TxnHandle* out_txn);

    /// Commit a transaction: append COMMITTED (retried on transient WAL failure,
    /// Q1-B), then mark done and possibly compact (Q3). Returns
    /// CX_ERR_PWL_TXN_NOT_FOUND / _TXN_ALREADY_COMMITTED / _TXN_ALREADY_ROLLBACK
    /// per the state table (Q5), CX_ERR_PWL_WRITE_FAILED if every retry fails.
    Status Commit(TxnHandle txn);

    /// Roll back a transaction: append ROLLBACK then invoke the rollback
    /// callback to clean up partial writes. Same state-table guards as Commit;
    /// CX_ERR_PWL_ROLLBACK_CALLBACK_FAILED if the callback fails (the ROLLBACK
    /// record is still durably written so recovery will retry).
    Status Rollback(TxnHandle txn);

    // --- diagnostics ---

    /// Live transaction counts from pending.wal (design § 2.1).
    Result<WalStats> GetStats();

    /// Current pending.wal size in bytes — O(1), for F05 memory budgeting
    /// (design § 2.1 v1.0.1, GetPwlSizeBytes). Zero before Init().
    size_t GetPwlSizeBytes() const;

    /// Testing seam: force the next `n` pending.wal appends to fail, to drive
    /// the Commit retry path (Q1-B) deterministically. Forwards to the writer's
    /// fault injector; no-op before Init(). Not for production use.
    void SetWalAppendFailuresForTest(int n) {
        if (wal_) wal_->SetAppendFailures(n);
    }

private:
    // Per-transaction in-memory record (Q5). doc_id + block_ids are retained so
    // Rollback can hand the original PENDING payload to the cleanup callback
    // (block_ids drive the vector-index per-block delete). block_ids are dropped
    // once the txn finalizes (committed/rolled-back txns no longer need them),
    // keeping the table within the design § 2.1 memory budget.
    struct TxnState {
        PendingEntry::State state;
        std::string doc_id;
        std::vector<BlockId> block_ids;
    };

    // Look up + advance the in-memory state for a finalizing op (Commit/
    // Rollback). On success the entry's state is updated so a repeat is
    // rejected, and *out is set to the original PENDING payload (doc_id +
    // block_ids) for doc-id release / rollback cleanup. Caller holds
    // txn_map_mu_. Returns an error Status for the bad-state cases (Q5).
    Status CheckAndMarkFinalizing(uint64_t txn_id, PendingEntry::State new_state,
                                  TxnState* out);

    // Recover one "PENDING-only" txn via the three-way consistency check
    // (Q1-C). Returns true if the data is fully present (infer COMMITTED, keep);
    // false if any store is missing it (caller replays cleanup).
    bool AllStoresHave(const PendingEntry& pending);

    std::string data_dir_;
    WriteCoordinatorConfig config_;

    // Borrowed storage backends for Recover()'s three-way check (design § 4.4).
    IVectorStore* vector_store_;
    IMetadataStore* metadata_store_;
    IBlobStore* blob_store_;

    std::unique_ptr<PendingLogWriter> wal_;
    RollbackCallback rollback_cb_;
    std::atomic<uint64_t> next_txn_id_{1};

    // State tables (Q5/Q2). Guarded by txn_map_mu_.
    std::mutex txn_map_mu_;
    std::unordered_map<uint64_t, TxnState> active_txns_;     // txn_id → state + payload
    std::unordered_map<std::string, uint64_t> active_docs_;  // doc_id → txn_id
    uint32_t committed_since_compact_ = 0;  // Q3 inline-compact counter
};

}  // namespace cortrix::store
