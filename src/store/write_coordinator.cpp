#include "cortrix/store/write_coordinator.h"

#include <algorithm>
#include <chrono>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "cortrix/store/pwl_errors.h"

namespace cortrix::store {

namespace {

int64_t NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

}  // namespace

WriteCoordinator::WriteCoordinator(const std::string& data_dir,
                                   const WriteCoordinatorConfig& config,
                                   IVectorStore* vector_store,
                                   IMetadataStore* metadata_store,
                                   IBlobStore* blob_store)
    : data_dir_(data_dir),
      config_(config),
      vector_store_(vector_store),
      metadata_store_(metadata_store),
      blob_store_(blob_store) {}

WriteCoordinator::~WriteCoordinator() = default;

Status WriteCoordinator::Init() {
    const std::string path = data_dir_ + "/pending.wal";
    Result<std::unique_ptr<PendingLogWriter>> w = PendingLogWriter::Open(
        path, config_.group_commit_max_batch, config_.group_commit_timeout_ms);
    if (!w.ok()) return w.status();
    wal_ = std::move(w.value());
    return Status::Ok();
}

void WriteCoordinator::SetRollbackCallback(RollbackCallback cb) {
    rollback_cb_ = std::move(cb);
}

bool WriteCoordinator::AllStoresHave(const PendingEntry& pending) {
    // Three-way consistency check (design § 4.4 Q1-C). A "PENDING-only" txn is
    // inferred COMMITTED only if every store already holds the data — meaning
    // the three-way write finished and only the COMMITTED WAL record was lost.
    // Any single miss means the crash landed mid-write → not committed.
    // Blob is required only when the txn declared it writes one (§4.4 v1.0.2): a
    // blob-less doc (watch_dir/CDC/memory) legitimately has none, so its absence
    // must NOT be read as an incomplete write — that was the §10.4 data-loss bug.
    if (pending.has_blob && !blob_store_->BlobExists(pending.doc_id)) return false;
    if (!pending.block_ids.empty() && !metadata_store_->BlockExists(pending.block_ids)) {
        return false;
    }
    for (BlockId b : pending.block_ids) {
        if (!vector_store_->Exists(static_cast<uint64_t>(b))) return false;
    }
    return true;
}

Status WriteCoordinator::Recover() {
    if (!wal_) {
        return PwlStatus(StatusCode::kUnavailable, pwl_errors::kWriteFailed,
                         "WriteCoordinator not initialized");
    }
    if (!vector_store_ || !metadata_store_ || !blob_store_) {
        return PwlStatus(StatusCode::kInvalidArgument, pwl_errors::kRecoveryFailed,
                         "Recover requires vector/metadata/blob stores");
    }

    // 1. Read every record in EOF order (a corrupt tail is already truncated by
    //    ReadAll → degraded recovery, design § 5).
    Result<std::vector<PendingEntry>> all = wal_->ReadAll();
    if (!all.ok()) return all.status();
    const std::vector<PendingEntry>& entries = all.value();

    if (static_cast<int>(entries.size()) > config_.max_recovery_entries) {
        return PwlStatus(StatusCode::kInternal, pwl_errors::kRecoveryFailed,
                         "pending.wal record count exceeds max_recovery_entries");
    }

    // 2. Group by txn_id, preserving the originating PENDING payload and noting
    //    the terminal state if any. (Recovery is single-threaded at startup, so
    //    no lock is needed while we build this.)
    struct TxnRecord {
        bool has_pending = false;
        bool has_committed = false;
        bool has_rollback = false;
        PendingEntry pending;  // valid iff has_pending
    };
    std::unordered_map<uint64_t, TxnRecord> by_txn;
    uint64_t max_txn = 0;
    for (const auto& e : entries) {
        TxnRecord& r = by_txn[e.txn_id];
        max_txn = std::max(max_txn, e.txn_id);
        switch (e.state) {
            case PendingEntry::State::kPending:
                r.has_pending = true;
                r.pending = e;
                break;
            case PendingEntry::State::kCommitted:
                r.has_committed = true;
                break;
            case PendingEntry::State::kRollback:
                r.has_rollback = true;
                break;
        }
    }

    // 3. Resolve each txn. We finalize every non-committed txn with a terminal
    //    record so the subsequent Compact can drop it (otherwise an inferred-
    //    COMMITTED PENDING-only txn would linger and be re-evaluated forever).
    for (const auto& kv : by_txn) {
        const uint64_t txn_id = kv.first;
        const TxnRecord& r = kv.second;
        if (r.has_committed) {
            continue;  // durable; nothing to do
        }

        // Decide cleanup vs infer-committed:
        //   - has_rollback                → cleanup (already durably ROLLBACK)
        //   - PENDING-only, stores missing → cleanup, then finalize as ROLLBACK
        //   - PENDING-only, stores present → infer COMMITTED, finalize as COMMITTED
        //   - terminal-only w/o pending    → nothing to undo
        bool needs_cleanup;
        bool infer_committed = false;
        if (r.has_rollback) {
            needs_cleanup = true;
        } else if (r.has_pending) {
            const bool present = AllStoresHave(r.pending);
            needs_cleanup = !present;
            infer_committed = present;
        } else {
            needs_cleanup = false;
        }

        if (needs_cleanup && rollback_cb_) {
            PendingEntry payload = r.has_pending ? r.pending : PendingEntry{};
            payload.state = PendingEntry::State::kPending;
            payload.txn_id = txn_id;
            Status cb = rollback_cb_(payload);
            if (!cb.ok() && config_.strict_recovery) {
                return PwlStatus(StatusCode::kInternal, pwl_errors::kRecoveryFailed,
                                 "strict recovery: cleanup failed for txn " +
                                     std::to_string(txn_id) + ": " + cb.message());
            }
            // Lenient mode (default): idempotent cleanup → log-and-continue.
        }

        // Finalize a PENDING-only txn so Compact can reclaim it. (A txn that
        // already has a ROLLBACK record is finalized; don't re-append.)
        if (r.has_pending && !r.has_rollback) {
            PendingEntry term;
            term.state = infer_committed ? PendingEntry::State::kCommitted
                                         : PendingEntry::State::kRollback;
            term.txn_id = txn_id;
            term.timestamp_ms = NowMs();
            Status w = wal_->Append(term);
            if (!w.ok() && config_.strict_recovery) {
                return PwlStatus(StatusCode::kInternal, pwl_errors::kRecoveryFailed,
                                 "strict recovery: failed to finalize txn " +
                                     std::to_string(txn_id) + ": " + w.message());
            }
        }
    }

    // 4. Compact pending.wal (drop finalized records), reseed the txn counter,
    //    and clear the in-memory tables (they are rebuilt by future writes).
    Status c = wal_->Compact();
    if (!c.ok()) return c;

    next_txn_id_.store(max_txn + 1, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lk(txn_map_mu_);
        active_txns_.clear();
        active_docs_.clear();
        committed_since_compact_ = 0;
    }
    return Status::Ok();
}

Status WriteCoordinator::BeginWrite(const std::string& doc_id,
                                    const std::vector<BlockId>& block_ids,
                                    bool writes_blob, TxnHandle* out_txn) {
    if (!wal_) {
        return PwlStatus(StatusCode::kUnavailable, pwl_errors::kWriteFailed,
                         "WriteCoordinator not initialized");
    }

    uint64_t txn_id;
    {
        // Reserve the doc_id and allocate the txn under the lock so a second
        // concurrent BeginWrite for the same doc is rejected (Q2) before the
        // slow WAL append. The append itself runs outside the lock so the
        // shared GroupCommitWriter can still batch concurrent appends.
        std::lock_guard<std::mutex> lk(txn_map_mu_);
        if (active_docs_.find(doc_id) != active_docs_.end()) {
            return PwlStatus(StatusCode::kAlreadyExists, pwl_errors::kDocWriteInProgress,
                             "doc_id already has a live transaction: " + doc_id);
        }
        txn_id = next_txn_id_.fetch_add(1, std::memory_order_relaxed);
        active_txns_[txn_id] = TxnState{PendingEntry::State::kPending, doc_id, block_ids};
        active_docs_[doc_id] = txn_id;
    }

    PendingEntry e;
    e.state = PendingEntry::State::kPending;
    e.txn_id = txn_id;
    e.timestamp_ms = NowMs();
    e.doc_id = doc_id;
    e.block_ids = block_ids;
    e.has_blob = writes_blob;

    Status w = wal_->Append(e);
    if (!w.ok()) {
        // Append failed: undo the reservation so the doc_id is writable again
        // and the txn_id is not left dangling in the state table.
        std::lock_guard<std::mutex> lk(txn_map_mu_);
        active_txns_.erase(txn_id);
        active_docs_.erase(doc_id);
        return PwlStatus(StatusCode::kUnavailable, pwl_errors::kWriteFailed,
                         "append PENDING failed: " + w.message());
    }

    out_txn->txn_id = txn_id;
    out_txn->lsn = txn_id;  // Phase 1: no separate LSN (TD-PWL-HEADER-FIELDS)
    return Status::Ok();
}

Status WriteCoordinator::CheckAndMarkFinalizing(uint64_t txn_id,
                                                PendingEntry::State new_state,
                                                TxnState* out) {
    auto it = active_txns_.find(txn_id);
    if (it == active_txns_.end()) {
        return PwlStatus(StatusCode::kNotFound, pwl_errors::kTxnNotFound,
                         "no such transaction");
    }
    switch (it->second.state) {
        case PendingEntry::State::kCommitted:
            return PwlStatus(StatusCode::kAlreadyExists, pwl_errors::kTxnAlreadyCommitted,
                             "transaction already committed");
        case PendingEntry::State::kRollback:
            return PwlStatus(StatusCode::kAlreadyExists, pwl_errors::kTxnAlreadyRollback,
                             "transaction already rolled back");
        case PendingEntry::State::kPending:
            break;  // the only legal precondition for finalizing
    }
    *out = it->second;          // copy original payload (doc_id + block_ids)
    it->second.state = new_state;  // mark now so a concurrent repeat is rejected
    it->second.block_ids.clear();  // payload no longer needed once finalized
    return Status::Ok();
}

Status WriteCoordinator::Commit(TxnHandle txn) {
    if (!wal_) {
        return PwlStatus(StatusCode::kUnavailable, pwl_errors::kWriteFailed,
                         "WriteCoordinator not initialized");
    }

    TxnState orig;
    {
        std::lock_guard<std::mutex> lk(txn_map_mu_);
        Status s = CheckAndMarkFinalizing(txn.txn_id, PendingEntry::State::kCommitted, &orig);
        if (!s.ok()) return s;
    }

    PendingEntry e;
    e.state = PendingEntry::State::kCommitted;
    e.txn_id = txn.txn_id;
    e.timestamp_ms = NowMs();

    // Retry transient WAL write failures with increasing backoff (Q1-B). Even
    // if every retry fails the three-way stores already hold the data; recovery
    // (S3) infers COMMITTED from the three-way consistency check, so no data is
    // lost — we surface the error but the state stays COMMITTED in memory.
    Status w = Status::Ok();
    int delay_ms = config_.commit_retry_base_ms;
    for (int attempt = 0; attempt <= config_.commit_max_retries; ++attempt) {
        w = wal_->Append(e);
        if (w.ok()) break;
        if (attempt < config_.commit_max_retries) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
            delay_ms *= 10;
        }
    }
    if (!w.ok()) {
        // (R2-M6) The three-way stores already hold the data and recovery (S3) infers
        // COMMITTED from the consistency check, so the doc IS committed even though the
        // WAL record was lost. Release its active-doc lock here exactly like the success
        // path below — otherwise doc_id leaks in active_docs_ and can never be written
        // again (BeginWrite rejects an already-active doc_id).
        {
            std::lock_guard<std::mutex> lk(txn_map_mu_);
            if (!orig.doc_id.empty()) active_docs_.erase(orig.doc_id);
        }
        return PwlStatus(StatusCode::kUnavailable, pwl_errors::kWriteFailed,
                         "append COMMITTED failed after retries: " + w.message());
    }

    bool do_compact = false;
    {
        std::lock_guard<std::mutex> lk(txn_map_mu_);
        if (!orig.doc_id.empty()) active_docs_.erase(orig.doc_id);
        ++committed_since_compact_;
        // A non-positive threshold disables inline compaction entirely.
        if (config_.compact_threshold > 0 &&
            committed_since_compact_ >= static_cast<uint32_t>(config_.compact_threshold)) {
            committed_since_compact_ = 0;
            do_compact = true;
        }
    }

    if (do_compact) {
        // Inline compaction (Q3): drop finalized records from pending.wal and
        // the matching terminal entries from the in-memory table. A compaction
        // failure is non-fatal — the commit already succeeded — so we swallow
        // it (the next threshold or a restart retries).
        Status c = wal_->Compact();
        if (c.ok()) {
            std::lock_guard<std::mutex> lk(txn_map_mu_);
            for (auto it = active_txns_.begin(); it != active_txns_.end();) {
                if (it->second.state != PendingEntry::State::kPending) {
                    it = active_txns_.erase(it);
                } else {
                    ++it;
                }
            }
        }
    }
    return Status::Ok();
}

Status WriteCoordinator::Rollback(TxnHandle txn) {
    if (!wal_) {
        return PwlStatus(StatusCode::kUnavailable, pwl_errors::kWriteFailed,
                         "WriteCoordinator not initialized");
    }

    TxnState orig;
    {
        std::lock_guard<std::mutex> lk(txn_map_mu_);
        Status s = CheckAndMarkFinalizing(txn.txn_id, PendingEntry::State::kRollback, &orig);
        if (!s.ok()) return s;
    }

    // Durably record the rollback intent first, so recovery will re-run cleanup
    // even if the callback below fails or the process dies mid-cleanup.
    PendingEntry e;
    e.state = PendingEntry::State::kRollback;
    e.txn_id = txn.txn_id;
    e.timestamp_ms = NowMs();
    Status w = wal_->Append(e);
    if (!w.ok()) {
        return PwlStatus(StatusCode::kUnavailable, pwl_errors::kWriteFailed,
                         "append ROLLBACK failed: " + w.message());
    }

    // Release the doc_id now that the txn is durably terminal.
    {
        std::lock_guard<std::mutex> lk(txn_map_mu_);
        if (!orig.doc_id.empty()) active_docs_.erase(orig.doc_id);
    }

    // Invoke the cleanup callback with the original PENDING payload (doc_id +
    // block_ids) so the layer above can drop partial writes idempotently (Q6):
    // block_ids drive the vector-index per-block delete, doc_id the SQLite /
    // blob delete-by-doc. A callback failure leaves the txn durably ROLLBACK;
    // recovery retries cleanup on next startup.
    if (rollback_cb_) {
        PendingEntry payload;
        payload.state = PendingEntry::State::kPending;
        payload.txn_id = txn.txn_id;
        payload.doc_id = orig.doc_id;
        payload.block_ids = orig.block_ids;
        Status cb = rollback_cb_(payload);
        if (!cb.ok()) {
            return PwlStatus(StatusCode::kInternal, pwl_errors::kRollbackCallbackFailed,
                             "rollback callback failed: " + cb.message());
        }
    }
    return Status::Ok();
}

Result<WalStats> WriteCoordinator::GetStats() {
    if (!wal_) {
        return PwlStatus(StatusCode::kUnavailable, pwl_errors::kWriteFailed,
                         "WriteCoordinator not initialized");
    }
    // WalStats is the shared writer type — pass the counts straight through.
    return wal_->GetStats();
}

size_t WriteCoordinator::GetPwlSizeBytes() const {
    return wal_ ? wal_->SizeBytes() : 0;
}

}  // namespace cortrix::store
