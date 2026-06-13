#include <gtest/gtest.h>

#include <fcntl.h>
#include <unistd.h>

#include <cstdint>
#include <cstdlib>
#include <set>
#include <string>
#include <vector>

#include "cortrix/store/pending_entry.h"
#include "cortrix/store/pending_log_writer.h"
#include "cortrix/store/recover_stores.h"
#include "cortrix/store/i_vector_store.h"
#include "cortrix/store/write_coordinator.h"

namespace cortrix::store {
namespace {

using State = PendingEntry::State;

// --- Programmable store fakes -------------------------------------------------
// Recovery's three-way check (Q1-C) must observe each store's "present?" answer.
// These fakes let a test declare exactly which blocks / docs survived a crash.

class FakeVectorStore : public IVectorStore {
public:
    Status AddPoint(const float*, uint64_t, const observability::TraceContext*) override {
        return Status::Ok();
    }
    Status MarkDelete(uint64_t block_id, const observability::TraceContext*) override {
        present_.erase(block_id);
        return Status::Ok();
    }
    std::vector<std::pair<uint64_t, float>> Search(
        const float*, int, const observability::TraceContext*) override {
        return {};
    }
    bool Exists(uint64_t block_id, const observability::TraceContext*) override {
        return present_.count(block_id) > 0;
    }
    Status Snapshot(const observability::TraceContext*) override { return Status::Ok(); }
    Status Recover(const observability::TraceContext*) override { return Status::Ok(); }

    void AddPresent(uint64_t b) { present_.insert(b); }
    std::set<uint64_t> present_;
};

class FakeMetadataStore : public IMetadataStore {
public:
    bool BlockExists(const std::vector<BlockId>& block_ids) override {
        for (BlockId b : block_ids) {
            if (present_.count(b) == 0) return false;
        }
        return true;
    }
    void AddPresent(BlockId b) { present_.insert(b); }
    std::set<BlockId> present_;
};

class FakeBlobStore : public IBlobStore {
public:
    bool BlobExists(const std::string& doc_id) override { return present_.count(doc_id) > 0; }
    void AddPresent(const std::string& d) { present_.insert(d); }
    std::set<std::string> present_;
};

class RecoveryTest : public ::testing::Test {
protected:
    void SetUp() override {
        char tmpl[] = "/tmp/cortrix_wcrec_XXXXXX";
        char* d = ::mkdtemp(tmpl);
        ASSERT_NE(d, nullptr);
        dir_ = d;
        wal_path_ = dir_ + "/pending.wal";
    }
    void TearDown() override {
        ::unlink(wal_path_.c_str());
        ::unlink((wal_path_ + ".compact.tmp").c_str());
        ::rmdir(dir_.c_str());
    }

    // Craft a crash state by writing raw records to pending.wal, then closing.
    void SeedWal(const std::vector<PendingEntry>& records) {
        auto w = PendingLogWriter::Open(wal_path_, 16, 5);
        ASSERT_TRUE(w.ok());
        for (const auto& e : records) ASSERT_TRUE(w.value()->Append(e).ok());
        w.value()->Shutdown();  // flush; destructor closes fd
    }

    static PendingEntry Pending(uint64_t txn, const std::string& doc,
                                std::vector<BlockId> blocks, bool has_blob = false) {
        PendingEntry e;
        e.state = State::kPending;
        e.txn_id = txn;
        e.doc_id = doc;
        e.block_ids = std::move(blocks);
        e.has_blob = has_blob;  // v1.0.2: gates the Recover blob check (§4.4)
        return e;
    }
    static PendingEntry Terminal(State s, uint64_t txn) {
        PendingEntry e;
        e.state = s;
        e.txn_id = txn;
        return e;
    }

    std::string dir_;
    std::string wal_path_;
};

TEST_F(RecoveryTest, EmptyWalRecoversCleanly) {
    FakeVectorStore vs; FakeMetadataStore ms; FakeBlobStore bs;
    WriteCoordinator wc(dir_, {}, &vs, &ms, &bs);
    ASSERT_TRUE(wc.Init().ok());
    ASSERT_TRUE(wc.Recover().ok());

    // next_txn_id reseeds to 1 → first post-recovery txn is 1.
    TxnHandle t;
    ASSERT_TRUE(wc.BeginWrite("d", {1}, /*writes_blob=*/true, &t).ok());
    EXPECT_EQ(t.txn_id, 1u);
}

TEST_F(RecoveryTest, AllCommittedNoCleanup) {
    SeedWal({Pending(1, "d1", {1}), Terminal(State::kCommitted, 1),
             Pending(2, "d2", {2}), Terminal(State::kCommitted, 2)});

    FakeVectorStore vs; FakeMetadataStore ms; FakeBlobStore bs;
    WriteCoordinator wc(dir_, {}, &vs, &ms, &bs);
    int cleanups = 0;
    wc.SetRollbackCallback([&](const PendingEntry&) { ++cleanups; return Status::Ok(); });
    ASSERT_TRUE(wc.Init().ok());
    ASSERT_TRUE(wc.Recover().ok());

    EXPECT_EQ(cleanups, 0);  // committed txns need no cleanup
    auto stats = wc.GetStats();
    ASSERT_TRUE(stats.ok());
    EXPECT_EQ(stats->pending, 0u);
    EXPECT_EQ(stats->committed, 0u);  // compacted away
}

TEST_F(RecoveryTest, AllRollbackReplaysCleanup) {
    SeedWal({Pending(1, "d1", {1}), Terminal(State::kRollback, 1),
             Pending(2, "d2", {2, 3}), Terminal(State::kRollback, 2)});

    FakeVectorStore vs; FakeMetadataStore ms; FakeBlobStore bs;
    WriteCoordinator wc(dir_, {}, &vs, &ms, &bs);
    std::set<uint64_t> cleaned;
    wc.SetRollbackCallback([&](const PendingEntry& e) {
        cleaned.insert(e.txn_id);
        return Status::Ok();
    });
    ASSERT_TRUE(wc.Init().ok());
    ASSERT_TRUE(wc.Recover().ok());

    EXPECT_EQ(cleaned, (std::set<uint64_t>{1, 2}));  // both rolled-back txns cleaned
    auto stats = wc.GetStats();
    ASSERT_TRUE(stats.ok());
    EXPECT_EQ(stats->pending, 0u);
    EXPECT_EQ(stats->rollback, 0u);  // compacted away
}

TEST_F(RecoveryTest, PendingOnlyAllStoresPresentInfersCommitted) {
    // Crash AFTER the three-way write but BEFORE the COMMITTED record.
    SeedWal({Pending(7, "doc-7", {10, 11})});

    FakeVectorStore vs; FakeMetadataStore ms; FakeBlobStore bs;
    vs.AddPresent(10); vs.AddPresent(11);
    ms.AddPresent(10); ms.AddPresent(11);
    bs.AddPresent("doc-7");

    WriteCoordinator wc(dir_, {}, &vs, &ms, &bs);
    int cleanups = 0;
    wc.SetRollbackCallback([&](const PendingEntry&) { ++cleanups; return Status::Ok(); });
    ASSERT_TRUE(wc.Init().ok());
    ASSERT_TRUE(wc.Recover().ok());

    EXPECT_EQ(cleanups, 0) << "data fully present → infer COMMITTED, no rollback (Q1-C)";
    // Inferred-COMMITTED txn is finalized + compacted away.
    auto stats = wc.GetStats();
    ASSERT_TRUE(stats.ok());
    EXPECT_EQ(stats->pending, 0u);
}

TEST_F(RecoveryTest, PendingOnlyPartialStoresRollsBack) {
    // Crash mid three-way write of a blob-writing doc: vector + metadata present,
    // blob missing. has_blob=true ⇒ the missing blob IS an incomplete write.
    SeedWal({Pending(7, "doc-7", {10, 11}, /*has_blob=*/true)});

    FakeVectorStore vs; FakeMetadataStore ms; FakeBlobStore bs;
    vs.AddPresent(10); vs.AddPresent(11);
    ms.AddPresent(10); ms.AddPresent(11);
    // bs: doc-7 absent → not fully written.

    WriteCoordinator wc(dir_, {}, &vs, &ms, &bs);
    PendingEntry cleaned;
    int cleanups = 0;
    wc.SetRollbackCallback([&](const PendingEntry& e) {
        cleaned = e; ++cleanups; return Status::Ok();
    });
    ASSERT_TRUE(wc.Init().ok());
    ASSERT_TRUE(wc.Recover().ok());

    ASSERT_EQ(cleanups, 1) << "any store missing → rollback (Q1-C)";
    EXPECT_EQ(cleaned.doc_id, "doc-7");
    EXPECT_EQ(cleaned.block_ids, (std::vector<BlockId>{10, 11}));  // block_ids from PENDING record
}

TEST_F(RecoveryTest, BlobLessDocMissingBlobInfersCommitted) {
    // §10.4 data-loss fix: a blob-less doc (has_blob=false — watch_dir / CDC /
    // memory writes that never touch the blob store) legitimately has no blob.
    // With vector + metadata present, Recover MUST infer COMMITTED and NOT roll it
    // back, even though the blob store is empty. Pre-fix this silently deleted a
    // correctly-committed doc (unconditional BlobExists → false → cleanup).
    SeedWal({Pending(7, "doc-7", {10, 11}, /*has_blob=*/false)});

    FakeVectorStore vs; FakeMetadataStore ms; FakeBlobStore bs;
    vs.AddPresent(10); vs.AddPresent(11);
    ms.AddPresent(10); ms.AddPresent(11);
    // bs intentionally empty: a blob-less doc has no blob to find.

    WriteCoordinator wc(dir_, {}, &vs, &ms, &bs);
    int cleanups = 0;
    wc.SetRollbackCallback([&](const PendingEntry&) { ++cleanups; return Status::Ok(); });
    ASSERT_TRUE(wc.Init().ok());
    ASSERT_TRUE(wc.Recover().ok());

    EXPECT_EQ(cleanups, 0)
        << "blob-less doc with vector+metadata present → infer COMMITTED, no rollback (§10.4)";
    auto stats = wc.GetStats();
    ASSERT_TRUE(stats.ok());
    EXPECT_EQ(stats->pending, 0u);  // finalized + compacted away
}

TEST_F(RecoveryTest, PendingOnlyNoStoresRollsBack) {
    SeedWal({Pending(7, "doc-7", {10})});
    FakeVectorStore vs; FakeMetadataStore ms; FakeBlobStore bs;  // all empty
    WriteCoordinator wc(dir_, {}, &vs, &ms, &bs);
    int cleanups = 0;
    wc.SetRollbackCallback([&](const PendingEntry&) { ++cleanups; return Status::Ok(); });
    ASSERT_TRUE(wc.Init().ok());
    ASSERT_TRUE(wc.Recover().ok());
    EXPECT_EQ(cleanups, 1);
}

// AllStoresHave short-circuits on the metadata store: a blob-less doc whose
// vector points exist but whose metadata row is missing is an incomplete write →
// rollback (the !BlockExists branch, distinct from the blob and vector checks).
TEST_F(RecoveryTest, PendingOnlyMetadataMissingRollsBack) {
    SeedWal({Pending(7, "doc-7", {10, 11}, /*has_blob=*/false)});

    FakeVectorStore vs; FakeMetadataStore ms; FakeBlobStore bs;
    vs.AddPresent(10); vs.AddPresent(11);
    // ms left empty → BlockExists([10,11]) is false even though vectors exist.

    WriteCoordinator wc(dir_, {}, &vs, &ms, &bs);
    int cleanups = 0;
    wc.SetRollbackCallback([&](const PendingEntry&) { ++cleanups; return Status::Ok(); });
    ASSERT_TRUE(wc.Init().ok());
    ASSERT_TRUE(wc.Recover().ok());
    EXPECT_EQ(cleanups, 1) << "metadata missing → rollback";
}

// AllStoresHave short-circuits on the vector store: metadata present for every
// block but one vector point missing → incomplete write → rollback (the per-block
// Exists branch).
TEST_F(RecoveryTest, PendingOnlyVectorMissingRollsBack) {
    SeedWal({Pending(7, "doc-7", {10, 11}, /*has_blob=*/false)});

    FakeVectorStore vs; FakeMetadataStore ms; FakeBlobStore bs;
    ms.AddPresent(10); ms.AddPresent(11);
    vs.AddPresent(10);  // 11 absent in the vector store

    WriteCoordinator wc(dir_, {}, &vs, &ms, &bs);
    int cleanups = 0;
    wc.SetRollbackCallback([&](const PendingEntry&) { ++cleanups; return Status::Ok(); });
    ASSERT_TRUE(wc.Init().ok());
    ASSERT_TRUE(wc.Recover().ok());
    EXPECT_EQ(cleanups, 1) << "one vector point missing → rollback";
}

// A blob-less, block-less PENDING (e.g. a memory write with no vector points) with
// the blob absent still infers COMMITTED: the blob check is gated on has_blob and
// the block loops are empty, so AllStoresHave returns true with nothing to verify.
TEST_F(RecoveryTest, PendingOnlyNoBlocksNoBlobInfersCommitted) {
    SeedWal({Pending(7, "doc-7", {}, /*has_blob=*/false)});

    FakeVectorStore vs; FakeMetadataStore ms; FakeBlobStore bs;  // all empty
    WriteCoordinator wc(dir_, {}, &vs, &ms, &bs);
    int cleanups = 0;
    wc.SetRollbackCallback([&](const PendingEntry&) { ++cleanups; return Status::Ok(); });
    ASSERT_TRUE(wc.Init().ok());
    ASSERT_TRUE(wc.Recover().ok());
    EXPECT_EQ(cleanups, 0) << "no blocks + no blob → nothing missing → infer COMMITTED";
}

// Recover finalizes a resolved PENDING-only txn by appending a terminal record.
// If that append fails under strict_recovery, recovery aborts with RECOVERY_FAILED
// (the strict-mode finalize-failure branch, distinct from the cleanup-callback one).
TEST_F(RecoveryTest, StrictModeFailsWhenFinalizeAppendFails) {
    // PENDING-only, all stores present → infer COMMITTED → finalize via wal_->Append.
    SeedWal({Pending(7, "doc-7", {10})});
    FakeVectorStore vs; FakeMetadataStore ms; FakeBlobStore bs;
    vs.AddPresent(10); ms.AddPresent(10);  // present → infer committed, no cleanup cb

    WriteCoordinatorConfig cfg;
    cfg.strict_recovery = true;
    WriteCoordinator wc(dir_, cfg, &vs, &ms, &bs);
    ASSERT_TRUE(wc.Init().ok());

    wc.SetWalAppendFailuresForTest(1);  // fail the terminal-record append
    Status s = wc.Recover();
    EXPECT_FALSE(s.ok());
    EXPECT_NE(s.message().find("CX_ERR_PWL_RECOVERY_FAILED"), std::string::npos);

    wc.SetWalAppendFailuresForTest(0);
}

// The same finalize-append failure under the default lenient mode is swallowed:
// recovery still returns OK (the strict_recovery=false arm of the same branch).
TEST_F(RecoveryTest, LenientModeToleratesFinalizeAppendFailure) {
    SeedWal({Pending(7, "doc-7", {10})});
    FakeVectorStore vs; FakeMetadataStore ms; FakeBlobStore bs;
    vs.AddPresent(10); ms.AddPresent(10);

    WriteCoordinatorConfig cfg;
    cfg.strict_recovery = false;  // default; explicit for clarity
    WriteCoordinator wc(dir_, cfg, &vs, &ms, &bs);
    ASSERT_TRUE(wc.Init().ok());

    wc.SetWalAppendFailuresForTest(1);
    EXPECT_TRUE(wc.Recover().ok()) << "lenient mode tolerates a finalize-append failure";

    wc.SetWalAppendFailuresForTest(0);
}

TEST_F(RecoveryTest, MixedStates) {
    // 2 committed, 1 rolled back, 2 pending-only (one present, one missing).
    SeedWal({
        Pending(1, "d1", {1}), Terminal(State::kCommitted, 1),
        Pending(2, "d2", {2}), Terminal(State::kCommitted, 2),
        Pending(3, "d3", {3}), Terminal(State::kRollback, 3),
        Pending(4, "d4", {4}),   // pending-only, will be present
        Pending(5, "d5", {5}),   // pending-only, will be missing
    });

    FakeVectorStore vs; FakeMetadataStore ms; FakeBlobStore bs;
    vs.AddPresent(4); ms.AddPresent(4); bs.AddPresent("d4");  // txn 4 fully present

    WriteCoordinator wc(dir_, {}, &vs, &ms, &bs);
    std::set<uint64_t> cleaned;
    wc.SetRollbackCallback([&](const PendingEntry& e) {
        cleaned.insert(e.txn_id); return Status::Ok();
    });
    ASSERT_TRUE(wc.Init().ok());
    ASSERT_TRUE(wc.Recover().ok());

    // txn 3 (rollback) + txn 5 (pending, missing) cleaned; txn 4 inferred committed.
    EXPECT_EQ(cleaned, (std::set<uint64_t>{3, 5}));
    auto stats = wc.GetStats();
    ASSERT_TRUE(stats.ok());
    EXPECT_EQ(stats->pending, 0u);  // all finalized + compacted
}

TEST_F(RecoveryTest, NextTxnIdSeededPastMax) {
    SeedWal({Pending(100, "d", {1}), Terminal(State::kCommitted, 100)});
    FakeVectorStore vs; FakeMetadataStore ms; FakeBlobStore bs;
    WriteCoordinator wc(dir_, {}, &vs, &ms, &bs);
    wc.SetRollbackCallback([](const PendingEntry&) { return Status::Ok(); });
    ASSERT_TRUE(wc.Init().ok());
    ASSERT_TRUE(wc.Recover().ok());

    TxnHandle t;
    ASSERT_TRUE(wc.BeginWrite("new", {2}, /*writes_blob=*/true, &t).ok());
    EXPECT_EQ(t.txn_id, 101u) << "next_txn_id must clear the highest recovered id";
}

TEST_F(RecoveryTest, RecoverClearsActiveState) {
    SeedWal({Pending(1, "d1", {1}), Terminal(State::kRollback, 1)});
    FakeVectorStore vs; FakeMetadataStore ms; FakeBlobStore bs;
    WriteCoordinator wc(dir_, {}, &vs, &ms, &bs);
    wc.SetRollbackCallback([](const PendingEntry&) { return Status::Ok(); });
    ASSERT_TRUE(wc.Init().ok());
    ASSERT_TRUE(wc.Recover().ok());

    // After recovery the doc_id is free again (active_docs_ cleared).
    TxnHandle t;
    EXPECT_TRUE(wc.BeginWrite("d1", {9}, /*writes_blob=*/true, &t).ok());
}

TEST_F(RecoveryTest, StrictModeFailsOnCallbackError) {
    SeedWal({Pending(1, "d1", {1}), Terminal(State::kRollback, 1)});
    FakeVectorStore vs; FakeMetadataStore ms; FakeBlobStore bs;
    WriteCoordinatorConfig cfg;
    cfg.strict_recovery = true;
    WriteCoordinator wc(dir_, cfg, &vs, &ms, &bs);
    wc.SetRollbackCallback([](const PendingEntry&) {
        return Status::Internal("cleanup failed");
    });
    ASSERT_TRUE(wc.Init().ok());
    Status s = wc.Recover();
    EXPECT_FALSE(s.ok());
    EXPECT_NE(s.message().find("CX_ERR_PWL_RECOVERY_FAILED"), std::string::npos);
}

TEST_F(RecoveryTest, LenientModeContinuesOnCallbackError) {
    SeedWal({Pending(1, "d1", {1}), Terminal(State::kRollback, 1),
             Pending(2, "d2", {2}), Terminal(State::kRollback, 2)});
    FakeVectorStore vs; FakeMetadataStore ms; FakeBlobStore bs;
    WriteCoordinatorConfig cfg;
    cfg.strict_recovery = false;  // default; explicit for clarity
    WriteCoordinator wc(dir_, cfg, &vs, &ms, &bs);
    int calls = 0;
    wc.SetRollbackCallback([&](const PendingEntry&) {
        ++calls;
        return Status::Internal("cleanup failed");  // always fails
    });
    ASSERT_TRUE(wc.Init().ok());
    EXPECT_TRUE(wc.Recover().ok()) << "lenient mode tolerates cleanup failures";
    EXPECT_EQ(calls, 2) << "both rolled-back txns are still attempted";
}

TEST_F(RecoveryTest, MaxRecoveryEntriesExceeded) {
    std::vector<PendingEntry> records;
    for (uint64_t i = 1; i <= 6; ++i) records.push_back(Pending(i, "d", {static_cast<BlockId>(i)}));
    SeedWal(records);

    FakeVectorStore vs; FakeMetadataStore ms; FakeBlobStore bs;
    WriteCoordinatorConfig cfg;
    cfg.max_recovery_entries = 5;  // 6 records > 5 → abort
    WriteCoordinator wc(dir_, cfg, &vs, &ms, &bs);
    wc.SetRollbackCallback([](const PendingEntry&) { return Status::Ok(); });
    ASSERT_TRUE(wc.Init().ok());
    Status s = wc.Recover();
    EXPECT_FALSE(s.ok());
    EXPECT_NE(s.message().find("CX_ERR_PWL_RECOVERY_FAILED"), std::string::npos);
}

TEST_F(RecoveryTest, RequiresAllStores) {
    SeedWal({Pending(1, "d", {1})});
    WriteCoordinator wc(dir_, {}, nullptr, nullptr, nullptr);  // no stores
    ASSERT_TRUE(wc.Init().ok());
    Status s = wc.Recover();
    EXPECT_FALSE(s.ok());
    EXPECT_NE(s.message().find("CX_ERR_PWL_RECOVERY_FAILED"), std::string::npos);
}

TEST_F(RecoveryTest, CorruptedTailTruncatedThenRecovers) {
    // Two clean committed txns, then a torn write at the tail. Recovery's
    // ReadAll truncates the garbage, leaving the two committed txns to resolve
    // normally (design § 4.4 + § 5 degraded recovery).
    SeedWal({Pending(1, "d1", {1}), Terminal(State::kCommitted, 1),
             Pending(2, "d2", {2}), Terminal(State::kCommitted, 2)});
    int fd = ::open(wal_path_.c_str(), O_RDWR);
    ASSERT_GE(fd, 0);
    off_t end = ::lseek(fd, 0, SEEK_END);
    const uint8_t junk[7] = {0xDE, 0xAD, 0xBE, 0xEF, 0x11, 0x22, 0x33};
    ASSERT_EQ(::pwrite(fd, junk, sizeof(junk), end), static_cast<ssize_t>(sizeof(junk)));
    ::close(fd);

    FakeVectorStore vs; FakeMetadataStore ms; FakeBlobStore bs;
    WriteCoordinator wc(dir_, {}, &vs, &ms, &bs);
    int cleanups = 0;
    wc.SetRollbackCallback([&](const PendingEntry&) { ++cleanups; return Status::Ok(); });
    ASSERT_TRUE(wc.Init().ok());
    ASSERT_TRUE(wc.Recover().ok());
    EXPECT_EQ(cleanups, 0) << "both surviving txns are committed";

    auto stats = wc.GetStats();
    ASSERT_TRUE(stats.ok());
    EXPECT_EQ(stats->pending, 0u);
    EXPECT_EQ(stats->committed, 0u);  // compacted
}

TEST_F(RecoveryTest, IdempotentAcrossTwoRecoveries) {
    // A pending-only-missing txn: first recovery rolls it back + finalizes; a
    // second recovery must be a clean no-op (compacted, nothing left).
    SeedWal({Pending(1, "d1", {1})});
    FakeVectorStore vs; FakeMetadataStore ms; FakeBlobStore bs;  // all empty → missing

    WriteCoordinator wc(dir_, {}, &vs, &ms, &bs);
    int calls = 0;
    wc.SetRollbackCallback([&](const PendingEntry&) { ++calls; return Status::Ok(); });
    ASSERT_TRUE(wc.Init().ok());
    ASSERT_TRUE(wc.Recover().ok());
    EXPECT_EQ(calls, 1);

    // Re-open + recover again: the txn was finalized as ROLLBACK and compacted,
    // so there is nothing left to clean.
    WriteCoordinator wc2(dir_, {}, &vs, &ms, &bs);
    int calls2 = 0;
    wc2.SetRollbackCallback([&](const PendingEntry&) { ++calls2; return Status::Ok(); });
    ASSERT_TRUE(wc2.Init().ok());
    ASSERT_TRUE(wc2.Recover().ok());
    EXPECT_EQ(calls2, 0) << "second recovery is a no-op after compaction";
}

}  // namespace
}  // namespace cortrix::store
