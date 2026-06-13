#include <gtest/gtest.h>

#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "cortrix/store/pending_entry.h"
#include "cortrix/store/write_coordinator.h"

namespace cortrix::store {
namespace {

using State = PendingEntry::State;

// S2 exercises the coordinator's own state machine; the three storage backends
// are only consulted by Recover() (S3), so we pass nullptr here.
class WriteCoordinatorTest : public ::testing::Test {
protected:
    void SetUp() override {
        char tmpl[] = "/tmp/cortrix_wc_XXXXXX";
        char* d = ::mkdtemp(tmpl);
        ASSERT_NE(d, nullptr);
        dir_ = d;
    }
    void TearDown() override {
        ::unlink((dir_ + "/pending.wal").c_str());
        ::unlink((dir_ + "/pending.wal.compact.tmp").c_str());
        ::rmdir(dir_.c_str());
    }

    std::unique_ptr<WriteCoordinator> MakeCoordinator(WriteCoordinatorConfig cfg = {}) {
        auto wc = std::make_unique<WriteCoordinator>(dir_, cfg, nullptr, nullptr, nullptr);
        EXPECT_TRUE(wc->Init().ok());
        return wc;
    }

    std::string dir_;
};

TEST_F(WriteCoordinatorTest, BeginWriteGeneratesIncreasingTxnIds) {
    auto wc = MakeCoordinator();
    TxnHandle t1, t2, t3;
    ASSERT_TRUE(wc->BeginWrite("doc-a", {1, 2}, /*writes_blob=*/true, &t1).ok());
    ASSERT_TRUE(wc->BeginWrite("doc-b", {3}, /*writes_blob=*/true, &t2).ok());
    ASSERT_TRUE(wc->BeginWrite("doc-c", {}, /*writes_blob=*/true, &t3).ok());
    EXPECT_LT(t1.txn_id, t2.txn_id);
    EXPECT_LT(t2.txn_id, t3.txn_id);
    EXPECT_EQ(t1.lsn, t1.txn_id);  // Phase 1 lsn mirrors txn_id
}

TEST_F(WriteCoordinatorTest, BeginWritePersistsPending) {
    auto wc = MakeCoordinator();
    TxnHandle t;
    ASSERT_TRUE(wc->BeginWrite("doc-1", {10, 20, 30}, /*writes_blob=*/true, &t).ok());

    auto stats = wc->GetStats();
    ASSERT_TRUE(stats.ok());
    EXPECT_EQ(stats->pending, 1u);
    EXPECT_EQ(stats->committed, 0u);
    EXPECT_EQ(stats->rollback, 0u);
}

TEST_F(WriteCoordinatorTest, DuplicateDocIdRejected) {
    auto wc = MakeCoordinator();
    TxnHandle t1, t2;
    ASSERT_TRUE(wc->BeginWrite("dup", {1}, /*writes_blob=*/true, &t1).ok());
    Status s = wc->BeginWrite("dup", {2}, /*writes_blob=*/true, &t2);
    EXPECT_FALSE(s.ok());
    EXPECT_NE(s.message().find("CX_ERR_PWL_DOC_WRITE_IN_PROGRESS"), std::string::npos);
}

TEST_F(WriteCoordinatorTest, DocIdReusableAfterCommit) {
    auto wc = MakeCoordinator();
    TxnHandle t1;
    ASSERT_TRUE(wc->BeginWrite("d", {1}, /*writes_blob=*/true, &t1).ok());
    ASSERT_TRUE(wc->Commit(t1).ok());
    // After commit the doc_id is released → a new write is allowed.
    TxnHandle t2;
    EXPECT_TRUE(wc->BeginWrite("d", {2}, /*writes_blob=*/true, &t2).ok());
    EXPECT_NE(t1.txn_id, t2.txn_id);
}

TEST_F(WriteCoordinatorTest, DocIdReusableAfterRollback) {
    auto wc = MakeCoordinator();
    TxnHandle t1;
    ASSERT_TRUE(wc->BeginWrite("d", {1}, /*writes_blob=*/true, &t1).ok());
    ASSERT_TRUE(wc->Rollback(t1).ok());
    TxnHandle t2;
    EXPECT_TRUE(wc->BeginWrite("d", {2}, /*writes_blob=*/true, &t2).ok());
}

TEST_F(WriteCoordinatorTest, CommitWritesCommittedEntry) {
    auto wc = MakeCoordinator();
    TxnHandle t;
    ASSERT_TRUE(wc->BeginWrite("d", {1}, /*writes_blob=*/true, &t).ok());
    ASSERT_TRUE(wc->Commit(t).ok());

    auto stats = wc->GetStats();
    ASSERT_TRUE(stats.ok());
    EXPECT_EQ(stats->pending, 0u);    // PENDING now finalized
    EXPECT_EQ(stats->committed, 1u);
}

TEST_F(WriteCoordinatorTest, CommitWithoutBeginReturnsNotFound) {
    auto wc = MakeCoordinator();
    TxnHandle bogus{999, 999};
    Status s = wc->Commit(bogus);
    EXPECT_FALSE(s.ok());
    EXPECT_NE(s.message().find("CX_ERR_PWL_TXN_NOT_FOUND"), std::string::npos);
}

TEST_F(WriteCoordinatorTest, DoubleCommitRejected) {
    auto wc = MakeCoordinator();
    TxnHandle t;
    ASSERT_TRUE(wc->BeginWrite("d", {1}, /*writes_blob=*/true, &t).ok());
    ASSERT_TRUE(wc->Commit(t).ok());
    Status s = wc->Commit(t);
    EXPECT_FALSE(s.ok());
    EXPECT_NE(s.message().find("CX_ERR_PWL_TXN_ALREADY_COMMITTED"), std::string::npos);
}

TEST_F(WriteCoordinatorTest, CommitAfterRollbackRejected) {
    auto wc = MakeCoordinator();
    TxnHandle t;
    ASSERT_TRUE(wc->BeginWrite("d", {1}, /*writes_blob=*/true, &t).ok());
    ASSERT_TRUE(wc->Rollback(t).ok());
    Status s = wc->Commit(t);
    EXPECT_FALSE(s.ok());
    EXPECT_NE(s.message().find("CX_ERR_PWL_TXN_ALREADY_ROLLBACK"), std::string::npos);
}

TEST_F(WriteCoordinatorTest, RollbackAfterCommitRejected) {
    auto wc = MakeCoordinator();
    TxnHandle t;
    ASSERT_TRUE(wc->BeginWrite("d", {1}, /*writes_blob=*/true, &t).ok());
    ASSERT_TRUE(wc->Commit(t).ok());
    Status s = wc->Rollback(t);
    EXPECT_FALSE(s.ok());
    EXPECT_NE(s.message().find("CX_ERR_PWL_TXN_ALREADY_COMMITTED"), std::string::npos);
}

TEST_F(WriteCoordinatorTest, RollbackInvokesCallbackWithOriginalPayload) {
    auto wc = MakeCoordinator();
    std::string seen_doc;
    uint64_t seen_txn = 0;
    std::vector<BlockId> seen_blocks;
    int calls = 0;
    wc->SetRollbackCallback([&](const PendingEntry& e) {
        seen_doc = e.doc_id;
        seen_txn = e.txn_id;
        seen_blocks = e.block_ids;
        ++calls;
        return Status::Ok();
    });

    TxnHandle t;
    ASSERT_TRUE(wc->BeginWrite("doc-rb", {11, 22, 33}, /*writes_blob=*/true, &t).ok());
    ASSERT_TRUE(wc->Rollback(t).ok());
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(seen_doc, "doc-rb");
    EXPECT_EQ(seen_txn, t.txn_id);
    // block_ids must reach the callback so it can delete the vector points (Q6).
    EXPECT_EQ(seen_blocks, (std::vector<BlockId>{11, 22, 33}));

    auto stats = wc->GetStats();
    ASSERT_TRUE(stats.ok());
    EXPECT_EQ(stats->rollback, 1u);
}

TEST_F(WriteCoordinatorTest, RollbackCallbackFailureSurfaces) {
    auto wc = MakeCoordinator();
    wc->SetRollbackCallback([](const PendingEntry&) {
        return Status::Internal("cleanup blew up");
    });
    TxnHandle t;
    ASSERT_TRUE(wc->BeginWrite("d", {1}, /*writes_blob=*/true, &t).ok());
    Status s = wc->Rollback(t);
    EXPECT_FALSE(s.ok());
    EXPECT_NE(s.message().find("CX_ERR_PWL_ROLLBACK_CALLBACK_FAILED"), std::string::npos);

    // The txn is still durably ROLLBACK despite the callback failing.
    auto stats = wc->GetStats();
    ASSERT_TRUE(stats.ok());
    EXPECT_EQ(stats->rollback, 1u);
    // And it cannot be committed afterwards.
    EXPECT_FALSE(wc->Commit(t).ok());
}

TEST_F(WriteCoordinatorTest, RollbackWithoutCallbackIsOk) {
    auto wc = MakeCoordinator();  // no callback registered
    TxnHandle t;
    ASSERT_TRUE(wc->BeginWrite("d", {1}, /*writes_blob=*/true, &t).ok());
    EXPECT_TRUE(wc->Rollback(t).ok());
}

TEST_F(WriteCoordinatorTest, CommitRetriesOnTransientWalFailure) {
    WriteCoordinatorConfig cfg;
    cfg.commit_max_retries = 3;
    cfg.commit_retry_base_ms = 1;  // keep the backoff (1/10/100ms) short for the test
    auto wc = MakeCoordinator(cfg);
    TxnHandle t;
    ASSERT_TRUE(wc->BeginWrite("d", {1}, /*writes_blob=*/true, &t).ok());

    // Force the first 2 COMMITTED appends to fail; attempt #3 (within 3 retries)
    // succeeds → Commit returns OK (Q1-B).
    wc->SetWalAppendFailuresForTest(2);
    ASSERT_TRUE(wc->Commit(t).ok());

    auto stats = wc->GetStats();
    ASSERT_TRUE(stats.ok());
    EXPECT_EQ(stats->committed, 1u);
}

TEST_F(WriteCoordinatorTest, CommitFailsWhenRetriesExhausted) {
    WriteCoordinatorConfig cfg;
    cfg.commit_max_retries = 2;     // attempts = 3 total (initial + 2 retries)
    cfg.commit_retry_base_ms = 1;
    auto wc = MakeCoordinator(cfg);
    TxnHandle t;
    ASSERT_TRUE(wc->BeginWrite("d", {1}, /*writes_blob=*/true, &t).ok());

    // Fail more times than the retry budget → Commit surfaces the error.
    wc->SetWalAppendFailuresForTest(10);
    Status s = wc->Commit(t);
    EXPECT_FALSE(s.ok());
    EXPECT_NE(s.message().find("CX_ERR_PWL_WRITE_FAILED"), std::string::npos);

    // Drain any remaining injected faults so teardown / later ops are clean.
    wc->SetWalAppendFailuresForTest(0);
}

TEST_F(WriteCoordinatorTest, InlineCompactTriggersAtThreshold) {
    WriteCoordinatorConfig cfg;
    cfg.compact_threshold = 5;  // compact after every 5 commits
    auto wc = MakeCoordinator(cfg);

    // 5 begin+commit cycles → the 5th commit triggers inline compaction, which
    // drops the finalized records. After it, GetStats sees no committed records.
    for (int i = 0; i < 5; ++i) {
        TxnHandle t;
        ASSERT_TRUE(wc->BeginWrite("doc-" + std::to_string(i), {static_cast<cortrix::BlockId>(i + 1)}, /*writes_blob=*/true, &t).ok());
        ASSERT_TRUE(wc->Commit(t).ok());
    }
    auto stats = wc->GetStats();
    ASSERT_TRUE(stats.ok());
    EXPECT_EQ(stats->pending, 0u);
    EXPECT_EQ(stats->committed, 0u) << "inline compaction should have dropped committed records";

    // Coordinator still works after compaction.
    TxnHandle t;
    ASSERT_TRUE(wc->BeginWrite("after", {99}, /*writes_blob=*/true, &t).ok());
    ASSERT_TRUE(wc->Commit(t).ok());
}

TEST_F(WriteCoordinatorTest, InlineCompactKeepsUnfinalizedPending) {
    WriteCoordinatorConfig cfg;
    cfg.compact_threshold = 3;
    auto wc = MakeCoordinator(cfg);

    // Leave one long-lived PENDING open, then drive 3 commits to trip compaction.
    TxnHandle open_txn;
    ASSERT_TRUE(wc->BeginWrite("long-lived", {7}, /*writes_blob=*/true, &open_txn).ok());
    for (int i = 0; i < 3; ++i) {
        TxnHandle t;
        ASSERT_TRUE(wc->BeginWrite("c-" + std::to_string(i), {static_cast<cortrix::BlockId>(i + 1)}, /*writes_blob=*/true, &t).ok());
        ASSERT_TRUE(wc->Commit(t).ok());
    }
    auto stats = wc->GetStats();
    ASSERT_TRUE(stats.ok());
    EXPECT_EQ(stats->pending, 1u) << "the open PENDING survives compaction";
    EXPECT_EQ(stats->committed, 0u);

    // The surviving txn can still be committed.
    EXPECT_TRUE(wc->Commit(open_txn).ok());
}

TEST_F(WriteCoordinatorTest, GetPwlSizeBytesGrowsWithWrites) {
    auto wc = MakeCoordinator();
    size_t base = wc->GetPwlSizeBytes();
    EXPECT_GT(base, 0u);  // at least the 32B header

    TxnHandle t;
    ASSERT_TRUE(wc->BeginWrite("d", {1, 2, 3}, /*writes_blob=*/true, &t).ok());
    size_t after_begin = wc->GetPwlSizeBytes();
    EXPECT_GT(after_begin, base);

    ASSERT_TRUE(wc->Commit(t).ok());
    EXPECT_GT(wc->GetPwlSizeBytes(), after_begin);
}

TEST_F(WriteCoordinatorTest, ConcurrentBeginWriteDistinctDocs) {
    auto wc = MakeCoordinator();
    constexpr int kThreads = 8;
    constexpr int kPer = 20;
    std::mutex mu;
    std::unordered_set<uint64_t> txn_ids;

    std::vector<std::thread> ts;
    for (int t = 0; t < kThreads; ++t) {
        ts.emplace_back([&, t] {
            for (int i = 0; i < kPer; ++i) {
                TxnHandle h;
                std::string doc = "t" + std::to_string(t) + "-" + std::to_string(i);
                ASSERT_TRUE(wc->BeginWrite(doc, {static_cast<BlockId>(i)}, /*writes_blob=*/true, &h).ok());
                std::lock_guard<std::mutex> lk(mu);
                txn_ids.insert(h.txn_id);
            }
        });
    }
    for (auto& th : ts) th.join();

    EXPECT_EQ(txn_ids.size(), static_cast<size_t>(kThreads * kPer))
        << "every BeginWrite must get a unique txn_id";
    auto stats = wc->GetStats();
    ASSERT_TRUE(stats.ok());
    EXPECT_EQ(stats->pending, static_cast<uint64_t>(kThreads * kPer));
}

TEST_F(WriteCoordinatorTest, NotInitializedRejectsBeginWrite) {
    WriteCoordinatorConfig cfg;
    WriteCoordinator wc(dir_, cfg, nullptr, nullptr, nullptr);  // no Init()
    TxnHandle t;
    EXPECT_FALSE(wc.BeginWrite("d", {1}, /*writes_blob=*/true, &t).ok());
    EXPECT_EQ(wc.GetPwlSizeBytes(), 0u);
}

// --- Branch coverage: every public op rejects an uninitialized coordinator. ---

// Each entry point guards on `wal_` being null; without Init() they all return
// the CX_ERR_PWL_WRITE_FAILED "not initialized" Status (Recover returns the
// RECOVERY_FAILED variant).
TEST_F(WriteCoordinatorTest, NotInitializedRejectsAllOps) {
    WriteCoordinator wc(dir_, {}, nullptr, nullptr, nullptr);  // no Init()

    EXPECT_FALSE(wc.Recover().ok());

    TxnHandle bogus{1, 1};
    Status commit = wc.Commit(bogus);
    EXPECT_FALSE(commit.ok());
    EXPECT_NE(commit.message().find("CX_ERR_PWL_WRITE_FAILED"), std::string::npos);

    Status rollback = wc.Rollback(bogus);
    EXPECT_FALSE(rollback.ok());
    EXPECT_NE(rollback.message().find("CX_ERR_PWL_WRITE_FAILED"), std::string::npos);

    auto stats = wc.GetStats();
    EXPECT_FALSE(stats.ok());
    EXPECT_NE(stats.status().message().find("CX_ERR_PWL_WRITE_FAILED"), std::string::npos);
}

// BeginWrite's WAL append fails → the reservation is rolled back so the doc_id is
// writable again and the txn_id is not left dangling (the undo branch).
TEST_F(WriteCoordinatorTest, BeginWriteAppendFailureUndoesReservation) {
    auto wc = MakeCoordinator();

    // Inject one fault so the PENDING append inside BeginWrite fails.
    wc->SetWalAppendFailuresForTest(1);
    TxnHandle t;
    Status s = wc->BeginWrite("doc-undo", {1}, /*writes_blob=*/true, &t);
    EXPECT_FALSE(s.ok());
    EXPECT_NE(s.message().find("CX_ERR_PWL_WRITE_FAILED"), std::string::npos);

    // Reservation was undone: the same doc_id can be written again, and the failed
    // attempt left nothing in the WAL.
    wc->SetWalAppendFailuresForTest(0);
    TxnHandle t2;
    EXPECT_TRUE(wc->BeginWrite("doc-undo", {1}, /*writes_blob=*/true, &t2).ok());
    auto stats = wc->GetStats();
    ASSERT_TRUE(stats.ok());
    EXPECT_EQ(stats->pending, 1u);  // only the successful retry persisted
}

// Rollback's ROLLBACK-record append fails → the error surfaces and the callback
// is never reached (the append-failure branch in Rollback).
TEST_F(WriteCoordinatorTest, RollbackAppendFailureSurfaces) {
    auto wc = MakeCoordinator();
    int callbacks = 0;
    wc->SetRollbackCallback([&](const PendingEntry&) { ++callbacks; return Status::Ok(); });

    TxnHandle t;
    ASSERT_TRUE(wc->BeginWrite("d", {1}, /*writes_blob=*/true, &t).ok());

    wc->SetWalAppendFailuresForTest(1);  // fail the ROLLBACK append
    Status s = wc->Rollback(t);
    EXPECT_FALSE(s.ok());
    EXPECT_NE(s.message().find("CX_ERR_PWL_WRITE_FAILED"), std::string::npos);
    EXPECT_EQ(callbacks, 0) << "append failed before the cleanup callback runs";

    wc->SetWalAppendFailuresForTest(0);
}

// A non-positive compact_threshold disables inline compaction entirely: committed
// records accumulate and are never auto-dropped (the threshold-disabled branch).
TEST_F(WriteCoordinatorTest, NonPositiveThresholdDisablesInlineCompaction) {
    WriteCoordinatorConfig cfg;
    cfg.compact_threshold = 0;  // disabled
    auto wc = MakeCoordinator(cfg);

    for (int i = 0; i < 4; ++i) {
        TxnHandle t;
        ASSERT_TRUE(wc->BeginWrite("doc-" + std::to_string(i),
                                   {static_cast<BlockId>(i + 1)}, /*writes_blob=*/true, &t).ok());
        ASSERT_TRUE(wc->Commit(t).ok());
    }
    auto stats = wc->GetStats();
    ASSERT_TRUE(stats.ok());
    EXPECT_EQ(stats->committed, 4u) << "no inline compaction → committed records remain";
}

// Inline compaction is best-effort: when the threshold trips but wal_->Compact()
// fails (here, the data dir is made read-only so the compaction temp file can't be
// created), the failure is swallowed and Commit still returns OK. Covers the
// `c.ok()` false arm of the inline-compaction block.
TEST_F(WriteCoordinatorTest, CommitSwallowsInlineCompactionFailure) {
    WriteCoordinatorConfig cfg;
    cfg.compact_threshold = 1;  // every commit trips compaction
    auto wc = MakeCoordinator(cfg);

    TxnHandle t;
    ASSERT_TRUE(wc->BeginWrite("d", {1}, /*writes_blob=*/true, &t).ok());

    // Make the dir read-only so Compact()'s ".compact.tmp" open fails inside Commit.
    ASSERT_EQ(::chmod(dir_.c_str(), 0500), 0);
    Status s = wc->Commit(t);
    ::chmod(dir_.c_str(), 0700);  // restore for TearDown
    EXPECT_TRUE(s.ok()) << "a compaction failure must not fail the already-durable commit";

    // The commit itself was durable: the COMMITTED record is present (compaction,
    // which would have dropped it, failed and was swallowed).
    auto stats = wc->GetStats();
    ASSERT_TRUE(stats.ok());
    EXPECT_EQ(stats->committed, 1u);
}

}  // namespace
}  // namespace cortrix::store
