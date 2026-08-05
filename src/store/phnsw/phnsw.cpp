#include <cstdint>
#include "cortrix/store/phnsw.h"

#include <algorithm>
#include <exception>
#include <filesystem>
#include <future>
#include <utility>
#include <vector>

#include "cortrix/logging/logging.h"
#include "cortrix/store/group_commit_writer.h"
#include "cortrix/store/phnsw/snapshot_manager.h"
#include "cortrix/store/phnsw/wal_entry.h"
#include "cortrix/store/phnsw/wal_writer.h"
#include "cortrix/store/phnsw_errors.h"

// hnswlib graph (vendored shallow fork — src/store/phnsw/hnswlib/, header-only).
#include <hnswlib/hnswlib.h>

namespace cortrix::store {

// ─────────────────────────────────────────────────────────────────────────────
// Class framework + hnswlib fork + durable group-commit writes.
//
// S1 built a usable PHnsw on the vendored hnswlib graph (read paths + a direct
// AddPoint). S3 makes writes crash-safe: AddPoint/AddPoints/MarkDelete now frame
// a WalEntry and Submit() it to a shared GroupCommitWriter whose sink (ApplySink)
// fdatasyncs hnsw.wal and only then replays the batch into the graph under the
// write lock (design § 4.1). So a write returns once it is BOTH durable and
// visible, and concurrent writers coalesce into one fsync per batch.
//
// Snapshot/Recover wiring (loading state from hnsw.wal on open) is S4; the
// constructor opens the WAL but does not yet replay it. Full concurrency
// hardening + TSAN is S5.
// ─────────────────────────────────────────────────────────────────────────────

// Durable-then-apply sink behind the GroupCommitWriter (design § 4.1). The
// committer guarantees exactly one WriteBatch()+Sync() per coalesced batch
// (group_commit_writer.h), so this stashes the batch's raw frames in WriteBatch
// and consumes them in Sync: WriteBatch appends to hnsw.wal (no fsync), Sync
// fdatasyncs and — only if durable — decodes + applies the frames to the graph.
class PHnsw::ApplySink : public IWalSink {
public:
    ApplySink(PHnsw* owner, WalWriter* wal) : owner_(owner), wal_(wal) {}

    // GroupCommitWriter calls WriteBatch then Sync back-to-back on its flush
    // thread (exactly one pair per coalesced batch). We do nothing durable in
    // WriteBatch but stash the batch; Sync does WAL-write + fsync + graph-apply
    // as ONE critical section under the write lock. Keeping all three under the
    // lock makes the WAL's committed_lsn and the graph's applied_lsn advance
    // atomically, so a concurrent Snapshot (which takes the same write lock)
    // never observes a durably-logged-but-unapplied record (S4 correctness; the
    // alternative — writing the WAL outside the lock — opens a snapshot/truncate
    // race). Readers (shared_lock) still run concurrently with everything except
    // this short apply window.
    Status WriteBatch(const std::vector<std::vector<uint8_t>>& entries) override {
        pending_ = &entries;  // borrowed; valid until Sync() returns (same thread)
        return Status::Ok();
    }

    Status Sync() override {
        const std::vector<std::vector<uint8_t>>* batch = pending_;
        pending_ = nullptr;
        if (batch == nullptr || batch->empty()) {
            return Status::Ok();
        }
        std::unique_lock<std::shared_mutex> lock(owner_->mutex_);
        // 1. Append the batch to hnsw.wal and fdatasync it (durable).
        Status w = wal_->WriteBatch(*batch);
        if (!w.ok()) {
            return w;
        }
        Status s = wal_->Sync();
        if (!s.ok()) {
            return s;
        }
        // 2. WAL durable → replay the batch into the graph in WAL order.
        for (const std::vector<uint8_t>& frame : *batch) {
            Result<WalEntry> e = WalEntry::Deserialize(frame, owner_->config_.dim);
            if (!e.ok()) {
                // A frame we just serialized must round-trip; otherwise the graph
                // and WAL would diverge — surface as a hard error.
                return e.status();
            }
            owner_->ApplyEntryLocked(e.value());
        }
        // 3. Auto-snapshot check (design § 4.1 step 6): if the WAL has grown past
        //    its thresholds, ask the background thread to snapshot. Still under
        //    the write lock — only flips a flag + notifies, never blocks here.
        owner_->MaybeRequestSnapshotLocked();
        return Status::Ok();
    }

private:
    PHnsw* const owner_;
    WalWriter* const wal_;
    const std::vector<std::vector<uint8_t>>* pending_ = nullptr;
};

PHnsw::PHnsw(const std::string& unit_data_dir, const PhnswConfig& config)
    : config_(config), unit_data_dir_(unit_data_dir) {
    space_ = std::make_unique<hnswlib::L2Space>(static_cast<size_t>(config_.dim));
    // index_ is built by RecoverInternal (either loaded from the newest snapshot
    // or created empty), so it is NOT constructed here in S4.

    // Ensure the per-Unit data directory exists (WAL + snapshots land here).
    // std::error_code overload — never throws.
    std::error_code ec;
    std::filesystem::create_directories(unit_data_dir_, ec);

    snapshot_mgr_ = std::make_unique<SnapshotManager>(unit_data_dir_, config_.snapshot_keep_count);

    // Open hnsw.wal (S2). If it cannot be opened the index stays NOT READY and
    // writes are rejected (factory surfaces it).
    const std::string wal_path = (std::filesystem::path(unit_data_dir_) / "hnsw.wal").string();
    Result<std::unique_ptr<WalWriter>> wal = WalWriter::Open(wal_path, config_.dim);
    if (!wal.ok()) {
        CORTRIX_LOG_ERROR("phnsw", "WAL open failed at {}: {}", wal_path,
                          wal.status().message());
        ready_ = false;
        return;
    }
    wal_ = std::move(wal.value());

    // S4: load the newest snapshot + replay WAL (design § 4.4) before accepting
    // traffic (§ 4.4-ter READY gate). On failure the index stays NOT READY.
    Status rec = RecoverInternal();
    if (!rec.ok()) {
        CORTRIX_LOG_ERROR("phnsw", "Recover failed for {}: {}", unit_data_dir_, rec.message());
        ready_ = false;
        return;
    }

    // Wire the group-commit write path (S3) only once recovery has populated the
    // graph, so no AddPoint can race an in-progress replay.
    apply_sink_ = std::make_unique<ApplySink>(this, wal_.get());
    committer_ = std::make_unique<GroupCommitWriter>(
        apply_sink_.get(), config_.group_commit_max_batch, config_.group_commit_timeout_ms);

    // S6: start the background auto-snapshot thread (off the write path).
    snapshot_thread_ = std::thread(&PHnsw::SnapshotLoop, this);

    ready_ = true;
}

Status PHnsw::RecoverInternal() {
    // Phase-QA C1-1 guard: when WalWriter::Open fails in the ctor, wal_ stays null and
    // ready_=false. The public Recover() (factory Open path) calls this unconditionally —
    // without this guard the wal_->ReadAll() in § 4.4 step 2 below null-derefs (SIGSEGV)
    // on any unopenable/corrupt-header WAL (the recovery path should fail gracefully on
    // disk faults). Return a clean NOT_READY, consistent with the § 4.4-ter READY gate.
    if (!wal_) {
        return PhnswStatus(StatusCode::kUnavailable, phnsw_errors::kNotReady,
                           "Recover: WAL not open (index not ready); "
                           "WalWriter::Open failed during construction");
    }
    // 1. Load the newest valid snapshot into index_ (design § 4.4 step 1).
    uint64_t snapshot_lsn = 0;
    std::unique_ptr<hnswlib::HierarchicalNSW<float>> loaded;
    Status ls = snapshot_mgr_->Load(space_.get(), config_.max_elements, &loaded, &snapshot_lsn);
    if (!ls.ok()) {
        return ls;  // every snapshot corrupt -> CX_ERR_PHNSW_SNAPSHOT_CORRUPTED
    }
    if (loaded) {
        index_ = std::move(loaded);
        index_->setEf(config_.ef_search);
    } else {
        // No snapshot: start from an empty graph.
        index_ = std::make_unique<hnswlib::HierarchicalNSW<float>>(
            space_.get(), static_cast<size_t>(config_.max_elements),
            static_cast<size_t>(config_.M), static_cast<size_t>(config_.ef_construction),
            /*random_seed=*/100, /*allow_replace_deleted=*/true);
        index_->setEf(config_.ef_search);
    }
    // Seed deleted_count_ from the loaded graph, NOT 0. A snapshot persists its
    // marked-deleted elements (hnswlib saveIndex writes the DELETE_MARK bits, and
    // loadIndex rebuilds num_deleted_ by scanning them), so a "delete then
    // snapshot then reopen" leaves deleted elements in the snapshot that the WAL
    // replay below will NOT re-see (the DELETE record was truncated with the WAL
    // at snapshot time). Resetting to 0 under-counted deletions → SearchLocked's
    // live = total - deleted_count_ over-estimated → it could ask hnswlib for more
    // than the live count (Bug store-M1). getDeletedCount() returns 0 for a fresh
    // empty graph, so the no-snapshot path is unaffected.
    deleted_count_ = index_->getDeletedCount();
    applied_lsn_ = snapshot_lsn;

    // 2. Replay WAL records whose global LSN > snapshot_lsn (design § 4.4 step 2).
    //    A record's global LSN = base + position, where base is the LSN just
    //    before the first record currently in the file.
    Result<std::vector<WalEntry>> recs = wal_->ReadAll(config_.dim);
    if (!recs.ok()) {
        return recs.status();
    }
    const uint64_t base = wal_->committed_lsn() - wal_->entry_count();
    const std::vector<WalEntry>& entries = recs.value();
    for (size_t i = 0; i < entries.size(); ++i) {
        const uint64_t lsn = base + i + 1;
        if (lsn <= snapshot_lsn) {
            continue;  // already incorporated by the snapshot (defensive)
        }
        ApplyEntryLocked(entries[i]);  // bumps applied_lsn_ to lsn
    }
    return Status::Ok();
}

void PHnsw::MaybeRequestSnapshotLocked() {
    // Caller holds the write lock. Cheap threshold check on the WAL; if exceeded,
    // flag the background thread (coalescing). Never snapshots inline — that would
    // need the write lock we already hold and would stall the flush.
    if (!wal_) {
        return;
    }
    Result<WalWriterStats> s = wal_->GetStats();
    if (!s.ok()) {
        return;
    }
    const uint64_t entries = s.value().entry_count;
    const std::size_t bytes = s.value().file_size_bytes;
    const bool over_entries =
        config_.snapshot_max_wal_entries > 0 &&
        entries >= static_cast<uint64_t>(config_.snapshot_max_wal_entries);
    const bool over_bytes =
        config_.snapshot_max_wal_size_mb > 0 &&
        bytes >= static_cast<std::size_t>(config_.snapshot_max_wal_size_mb) * 1024u * 1024u;
    if (!over_entries && !over_bytes) {
        return;
    }
    {
        std::lock_guard<std::mutex> lk(snapshot_mu_);
        snapshot_requested_ = true;
    }
    snapshot_cv_.notify_one();
}

void PHnsw::SnapshotLoop() {
    for (;;) {
        {
            std::unique_lock<std::mutex> lk(snapshot_mu_);
            snapshot_cv_.wait(lk, [this] { return snapshot_requested_ || snapshot_stop_; });
            if (snapshot_stop_) {
                return;
            }
            snapshot_requested_ = false;  // consume; new crossings re-flag
        }
        // Snapshot() takes the write lock itself; running it here (not in the
        // flush) keeps writes unblocked except for the brief snapshot window.
        Status s = Snapshot();
        if (!s.ok()) {
            CORTRIX_LOG_WARN("phnsw", "auto-snapshot failed: {}", s.message());
            // Leave it; the next threshold crossing re-requests.
        }
    }
}

PHnsw::~PHnsw() {
    // Teardown order matters. (1) Stop the committer's worker thread first, so no
    // more writes/snapshot-requests are generated and nothing calls into
    // apply_sink_ -> wal_ during teardown. (2) Then stop the background snapshot
    // thread (it may be mid-Snapshot holding the write lock; we let it finish).
    // Members destruct in reverse declaration order (committer_, apply_sink_,
    // wal_, ... snapshot_mgr_, index_), consistent with this.
    if (committer_) {
        committer_->Shutdown();
    }
    {
        std::lock_guard<std::mutex> lk(snapshot_mu_);
        snapshot_stop_ = true;
    }
    snapshot_cv_.notify_all();
    if (snapshot_thread_.joinable()) {
        snapshot_thread_.join();
    }
}

void PHnsw::ApplyEntryLocked(const WalEntry& entry) {
    // Caller holds the write lock (mutex_), OR is the single-threaded constructor
    // recovery path (no committer yet). INSERT -> addPoint, DELETE -> markDelete
    // (idempotent). Exceptions from hnswlib are swallowed to keep replay total; a
    // genuinely fatal graph error would have shown up at WAL-decode time.
    // applied_lsn_ advances 1:1 with WAL order so a later Snapshot records exactly
    // the graph state it captured.
    try {
        if (entry.type == WalEntry::Type::kInsert) {
            ApplyInsertLocked(entry);
        } else {
            try {
                index_->markDelete(static_cast<hnswlib::labeltype>(entry.block_id));
                ++deleted_count_;
            } catch (const std::exception&) {
                // block_id absent OR already marked deleted — idempotent no-op
                // markDelete throws "Label not found" for an
                // absent id and "already deleted" for a repeated delete; both are
                // legitimately no-ops here (a re-delete must NOT double-count).
            }
        }
    } catch (const std::exception& e) {
        CORTRIX_LOG_ERROR("phnsw", "ApplyEntry failed (block_id={}): {}", entry.block_id,
                          e.what());
    }
    ++applied_lsn_;
}

void PHnsw::ApplyInsertLocked(const WalEntry& entry) {
    // Caller holds the write lock and is inside ApplyEntryLocked's try.
    //
    // An INSERT for a block_id that is currently MARKED DELETED is a re-write.
    // The index design (boundary table) sanctions this: a duplicate-block_id write
    // is "MarkDelete then AddPoint" with the same id (caller-guaranteed ordering),
    // because hnswlib disallows two live nodes with the same label.
    // The index is built with allow_replace_deleted_=true (RecoverInternal), and
    // under that flag hnswlib's 2-arg addPoint THROWS on a marked-deleted label
    // ("Can't use addPoint to update deleted elements if replacement of deleted
    // elements is enabled."). Before this branch that throw was swallowed by the
    // outer catch, so the INSERT's vector — already durable in hnsw.wal — never
    // entered the graph: silent data loss on every re-write (Bug store-C1).
    //
    // We therefore detect the marked-deleted case and resurrect the SAME label in
    // place: unmarkDelete clears the delete mark (decrementing num_deleted_), then
    // the 2-arg addPoint finds the now-live label and routes to updatePoint,
    // overwriting its vector. This keeps the block_id stable (vs. replace_deleted,
    // which grabs an arbitrary vacant slot and relabels it) and keeps
    // deleted_count_ in lock-step with hnswlib's num_deleted_.
    const auto label = static_cast<hnswlib::labeltype>(entry.block_id);
    if (IsLabelMarkedDeletedLocked(label)) {
        index_->unmarkDelete(label);  // clears DELETE_MARK, num_deleted_ -= 1
        if (deleted_count_ > 0) {
            --deleted_count_;
        }
    }
    // Live-or-absent label: addPoint inserts a new node, or (label already live —
    // a fresh INSERT for a still-present id, or the resurrected re-write above)
    // routes to updatePoint and overwrites the vector. No replace_deleted here, so
    // an unrelated marked-deleted slot is never silently relabeled to this id.
    index_->addPoint(entry.vector.data(), label);
}

bool PHnsw::IsLabelMarkedDeletedLocked(uint64_t label) const {
    // Caller holds mutex_. Probes hnswlib's label map (its own label_lookup_lock,
    // distinct from mutex_) for "present AND marked deleted" — the re-write
    // precondition. Mirrors Exists()'s membership probe.
    try {
        std::unique_lock<std::mutex> label_lock(index_->label_lookup_lock);
        auto it = index_->label_lookup_.find(static_cast<hnswlib::labeltype>(label));
        if (it == index_->label_lookup_.end()) {
            return false;  // absent
        }
        return index_->isMarkedDeleted(it->second);
    } catch (const std::exception&) {
        return false;
    }
}

// --- Writes ---------------------------------------------------------------

Status PHnsw::AddPoint(const float* vec, uint64_t block_id,
                       const observability::TraceContext* /*ctx*/) {
    // Step 1 (design § 4.1): validate dim and readiness up front — fail fast
    // WITHOUT touching the WAL.
    if (vec == nullptr) {
        return Status::InvalidArgument("PHnsw::AddPoint: null vector");
    }
    if (!ready_ || !committer_) {
        return PhnswStatus(StatusCode::kUnavailable, phnsw_errors::kNotReady,
                           "PHnsw::AddPoint: index not ready");
    }
    // Step 2: frame the INSERT record. Step 3: Submit to the group committer.
    // Step 5: block on the batch's durability+apply future (the sink applies to
    // the graph after fdatasync, before resolving — see ApplySink::Sync).
    std::vector<uint8_t> frame = WalEntry::SerializeInsert(block_id, vec, config_.dim);
    std::future<Status> fut = committer_->Submit(std::move(frame));
    return fut.get();
}

Status PHnsw::AddPoints(const std::vector<std::pair<const float*, uint64_t>>& points,
                        const observability::TraceContext* /*ctx*/) {
    if (!ready_ || !committer_) {
        return PhnswStatus(StatusCode::kUnavailable, phnsw_errors::kNotReady,
                           "PHnsw::AddPoints: index not ready");
    }
    if (points.empty()) {
        return Status::Ok();
    }
    // Submit every record, then wait on all futures. Concurrent submits (these
    // plus other threads') coalesce into shared group-commit batches, so the
    // whole call typically costs far fewer than points.size() fsyncs.
    std::vector<std::future<Status>> futures;
    futures.reserve(points.size());
    for (const auto& [vec, block_id] : points) {
        if (vec == nullptr) {
            return Status::InvalidArgument("PHnsw::AddPoints: null vector in batch");
        }
        std::vector<uint8_t> frame = WalEntry::SerializeInsert(block_id, vec, config_.dim);
        futures.push_back(committer_->Submit(std::move(frame)));
    }
    Status first_err = Status::Ok();
    for (auto& f : futures) {
        Status s = f.get();
        if (!s.ok() && first_err.ok()) {
            first_err = s;
        }
    }
    return first_err;
}

Status PHnsw::MarkDelete(uint64_t block_id, const observability::TraceContext* /*ctx*/) {
    // Idempotent at the graph level (a missing block_id replays to a no-op),
    // but the DELETE is still journaled so recovery is exact.
    if (!ready_ || !committer_) {
        return PhnswStatus(StatusCode::kUnavailable, phnsw_errors::kNotReady,
                           "PHnsw::MarkDelete: index not ready");
    }
    std::vector<uint8_t> frame = WalEntry::SerializeDelete(block_id);
    std::future<Status> fut = committer_->Submit(std::move(frame));
    return fut.get();
}

// --- Search ---------------------------------------------------------------

std::vector<std::pair<uint64_t, float>> PHnsw::Search(
    const float* query, int top_k, int ef_search,
    const observability::TraceContext* /*ctx*/) {
    std::vector<std::pair<uint64_t, float>> out;
    if (query == nullptr || top_k <= 0) {
        return out;
    }
    // Concurrency (S5): hnswlib searchKnn is safe for concurrent readers (its
    // visited-list pool is mutex-guarded and the metric counters are atomic), so
    // the common default-ef path runs under a shared_lock — many searches in
    // parallel. A per-query ef_search, however, can only be applied via the
    // shared member setEf(), which mutates ef_; doing that under a shared_lock
    // would be a data race (and would bleed ef into other concurrent searches).
    // So an explicit ef_search takes the exclusive lock for the duration of that
    // one search. ef_search <= 0 (the default, what ScatterGather uses) keeps
    // full read concurrency.
    if (ef_search > 0) {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        if (!index_) {
            return out;
        }
        const size_t saved_ef = index_->ef_;
        index_->setEf(static_cast<size_t>(ef_search));
        out = SearchLocked(query, top_k);
        index_->setEf(saved_ef);  // restore so it does not leak to default-ef searches
        return out;
    }
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return SearchLocked(query, top_k);
}

std::vector<std::pair<uint64_t, float>> PHnsw::SearchLocked(const float* query, int top_k) {
    // Caller holds mutex_ (shared or unique). Pure read of the graph.
    std::vector<std::pair<uint64_t, float>> out;
    if (!index_ || index_->getCurrentElementCount() == 0) {
        return out;
    }
    try {
        const size_t total = index_->getCurrentElementCount();
        const size_t live = (total > deleted_count_) ? (total - deleted_count_) : 0;
        const size_t actual_k = std::min(static_cast<size_t>(top_k), live);
        if (actual_k == 0) {
            return out;
        }
        auto pq = index_->searchKnn(query, actual_k);
        out.reserve(pq.size());
        while (!pq.empty()) {
            const auto& top = pq.top();  // {distance, label}
            out.emplace_back(static_cast<uint64_t>(top.second), top.first);
            pq.pop();
        }
        // hnswlib returns a max-heap (farthest first); reverse to closest-first.
        std::reverse(out.begin(), out.end());
    } catch (const std::exception& e) {
        CORTRIX_LOG_ERROR("phnsw", "Search failed: {}", e.what());
        out.clear();
    }
    return out;
}

// --- IIndex membership / persistence / lifecycle --------------------------

bool PHnsw::Exists(uint64_t block_id) {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    if (!index_) {
        return false;
    }
    // getExternalLabel scans by internal id, so use hnswlib's label map: a label
    // is present iff it is in label_lookup_ and not marked deleted.
    try {
        std::unique_lock<std::mutex> label_lock(index_->label_lookup_lock);
        auto it = index_->label_lookup_.find(static_cast<hnswlib::labeltype>(block_id));
        if (it == index_->label_lookup_.end()) {
            return false;
        }
        const bool deleted = index_->isMarkedDeleted(it->second);
        return !deleted;
    } catch (const std::exception&) {
        return false;
    }
}

Status PHnsw::Snapshot() {
    // design § 4.3: take the write lock (blocks writes + reads for the snapshot
    // window, Phase 1 — COW is deferred to Phase 2), persist the graph stamped
    // with applied_lsn_ (which exactly matches the graph state under this lock),
    // then truncate the WAL. SnapshotManager::Save also prunes old snapshots.
    std::unique_lock<std::shared_mutex> lock(mutex_);
    if (!snapshot_mgr_ || !index_) {
        return PhnswStatus(StatusCode::kUnavailable, phnsw_errors::kSnapshotFailed,
                           "PHnsw::Snapshot: index not initialized");
    }
    Status s = snapshot_mgr_->Save(index_.get(), applied_lsn_, config_.dim, config_.M);
    if (!s.ok()) {
        return s;
    }
    // WAL records up to applied_lsn_ are now captured by the snapshot → truncate.
    // committed_lsn stays monotonic (WalWriter::Truncate preserves it), so a
    // future snapshot's lsn and the next WAL records keep ascending consistently.
    if (wal_) {
        Status t = wal_->Truncate();
        if (!t.ok()) {
            return t;  // snapshot is durable; the stale WAL just gets re-truncated next time
        }
    }
    return Status::Ok();
}

Status PHnsw::Recover() {
    // Reload from disk: newest snapshot + WAL replay (design § 4.4). Used by the
    // factory Open path and after a fault. Takes the write lock so it cannot race
    // a concurrent write/search. (The constructor runs RecoverInternal once
    // before the committer exists; this public entry re-runs it safely.)
    std::unique_lock<std::shared_mutex> lock(mutex_);
    return RecoverInternal();
}

Status PHnsw::Shutdown() {
    // Flush + stop the group committer so all queued writes are durably synced
    // and applied before we return (S3). A final Snapshot is added in S4/S6.
    // Idempotent: GroupCommitWriter::Shutdown can be called more than once.
    if (committer_) {
        committer_->Shutdown();
    }
    return Status::Ok();
}

IndexStats PHnsw::GetStats() {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    IndexStats stats;
    stats.dim = config_.dim;
    if (index_) {
        const size_t total = index_->getCurrentElementCount();
        stats.vector_count = (total > deleted_count_) ? (total - deleted_count_) : 0;
        stats.deleted_count = deleted_count_;
    }
    stats.memory_footprint_bytes = GetMemoryFootprintBytesLocked();  // already under shared_lock
    return stats;
}

std::size_t PHnsw::GetMemoryFootprintBytes() const {
    // Public entry: take the read lock so a direct pool call is race-free against
    // concurrent writers (S5). GetStats() uses the *Locked variant instead, since
    // re-acquiring shared_lock on the same thread is UB for std::shared_mutex.
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return GetMemoryFootprintBytesLocked();
}

std::size_t PHnsw::GetMemoryFootprintBytesLocked() const {
    // Coarse upper-bound estimate for pool admission control. Caller holds
    // mutex_ (shared or exclusive). O(1) arithmetic.
    if (!index_) {
        return 0;
    }
    const std::size_t vector_count = index_->getCurrentElementCount();
    const std::size_t dim = static_cast<std::size_t>(config_.dim);
    const std::size_t m = static_cast<std::size_t>(config_.M);

    const std::size_t vec_bytes = vector_count * dim * sizeof(float);
    const std::size_t graph_bytes = vector_count * m * sizeof(uint32_t);
    const std::size_t metadata_bytes =
        sizeof(hnswlib::HierarchicalNSW<float>) + vector_count * sizeof(uint64_t);
    return vec_bytes + graph_bytes + metadata_bytes;
}

// --- IVectorStore subset (distinct signatures; delegate to the IIndex side) --

Status PHnsw::Snapshot(const observability::TraceContext* /*ctx*/) {
    return Snapshot();
}

Status PHnsw::Recover(const observability::TraceContext* /*ctx*/) {
    return Recover();
}

bool PHnsw::Exists(uint64_t block_id, const observability::TraceContext* /*ctx*/) {
    return Exists(block_id);
}

std::vector<std::pair<uint64_t, float>> PHnsw::Search(
    const float* query, int top_k, const observability::TraceContext* ctx) {
    // The write coordinator's no-ef_search overload uses the configured default search width.
    return Search(query, top_k, /*ef_search=*/-1, ctx);
}

// --- PHnsw-specific -------------------------------------------------------

PhnswWalStats PHnsw::GetWalStats() {
    PhnswWalStats out;
    if (!wal_) {
        return out;
    }
    Result<WalWriterStats> s = wal_->GetStats();
    if (s.ok()) {
        out.entry_count = s.value().entry_count;
        out.committed_lsn = s.value().committed_lsn;
        out.file_size_bytes = s.value().file_size_bytes;
    }
    return out;
}

}  // namespace cortrix::store
