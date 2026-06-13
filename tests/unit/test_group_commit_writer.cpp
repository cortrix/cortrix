#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "cortrix/store/group_commit_writer.h"

namespace cortrix::store {
namespace {

using namespace std::chrono_literals;

// Thread-safe recording fake for IWalSink. Records batch/sync counts and can be
// configured to fail WriteBatch or Sync, so tests can assert the group-commit
// invariants (one Sync per batch, durability handoff, error propagation).
class RecordingWalSink : public IWalSink {
public:
    Status WriteBatch(const std::vector<std::vector<uint8_t>>& entries) override {
        std::lock_guard<std::mutex> lk(mu_);
        if (fail_write_) return Status::Unavailable("disk full");
        ++write_batches_;
        last_batch_size_ = entries.size();
        total_entries_ += entries.size();
        return Status::Ok();
    }
    Status Sync() override {
        std::lock_guard<std::mutex> lk(mu_);
        if (fail_sync_) return Status::Internal("fsync failed");
        ++syncs_;
        return Status::Ok();
    }

    void set_fail_write(bool v) { std::lock_guard<std::mutex> lk(mu_); fail_write_ = v; }
    void set_fail_sync(bool v)  { std::lock_guard<std::mutex> lk(mu_); fail_sync_ = v; }

    int write_batches()      { std::lock_guard<std::mutex> lk(mu_); return write_batches_; }
    int syncs()              { std::lock_guard<std::mutex> lk(mu_); return syncs_; }
    std::size_t total()      { std::lock_guard<std::mutex> lk(mu_); return total_entries_; }
    std::size_t last_batch() { std::lock_guard<std::mutex> lk(mu_); return last_batch_size_; }

private:
    std::mutex mu_;
    int write_batches_ = 0;
    int syncs_ = 0;
    std::size_t total_entries_ = 0;
    std::size_t last_batch_size_ = 0;
    bool fail_write_ = false;
    bool fail_sync_ = false;
};

constexpr auto kWait = 2s;  // generous bound so the test never hangs on slow CI

TEST(Crc32Test, KnownVectors) {
    // Canonical IEEE CRC32 check value for "123456789".
    const std::string s = "123456789";
    EXPECT_EQ(Crc32(reinterpret_cast<const uint8_t*>(s.data()), s.size()), 0xCBF43926u);
    // Empty input is the identity 0 and must not dereference the (null) pointer.
    EXPECT_EQ(Crc32(nullptr, 0), 0u);
    // A single byte differs from empty.
    const uint8_t one = 'a';
    EXPECT_NE(Crc32(&one, 1), 0u);
}

TEST(GroupCommitWriterTest, SingleSubmitFlushesAndSyncs) {
    RecordingWalSink sink;
    GroupCommitWriter w(&sink, /*max_batch=*/16, /*timeout_ms=*/10);

    auto fut = w.Submit({1, 2, 3});
    ASSERT_EQ(fut.wait_for(kWait), std::future_status::ready);
    EXPECT_TRUE(fut.get().ok());

    EXPECT_EQ(sink.write_batches(), 1);
    EXPECT_EQ(sink.syncs(), 1);          // group-commit invariant: one fsync per batch
    EXPECT_EQ(sink.total(), 1u);
}

TEST(GroupCommitWriterTest, BatchPreservesSyncPerBatchInvariant) {
    RecordingWalSink sink;
    GroupCommitWriter w(&sink, /*max_batch=*/8, /*timeout_ms=*/50);

    std::vector<std::future<Status>> futs;
    for (int i = 0; i < 8; ++i) futs.push_back(w.Submit({static_cast<uint8_t>(i)}));
    for (auto& f : futs) {
        ASSERT_EQ(f.wait_for(kWait), std::future_status::ready);
        EXPECT_TRUE(f.get().ok());
    }

    EXPECT_EQ(sink.total(), 8u);                       // every entry durable
    EXPECT_GE(sink.write_batches(), 1);
    EXPECT_LE(sink.write_batches(), 8);
    EXPECT_EQ(sink.syncs(), sink.write_batches());     // exactly one Sync per batch
}

TEST(GroupCommitWriterTest, WriteFailurePropagatesAndSkipsSync) {
    RecordingWalSink sink;
    sink.set_fail_write(true);
    GroupCommitWriter w(&sink, /*max_batch=*/4, /*timeout_ms=*/10);

    auto fut = w.Submit({9});
    ASSERT_EQ(fut.wait_for(kWait), std::future_status::ready);
    EXPECT_FALSE(fut.get().ok());
    EXPECT_EQ(sink.syncs(), 0);  // never sync a batch whose write failed
}

TEST(GroupCommitWriterTest, SyncFailurePropagates) {
    RecordingWalSink sink;
    sink.set_fail_sync(true);
    GroupCommitWriter w(&sink, /*max_batch=*/4, /*timeout_ms=*/10);

    auto fut = w.Submit({7});
    ASSERT_EQ(fut.wait_for(kWait), std::future_status::ready);
    Status st = fut.get();
    EXPECT_FALSE(st.ok());
    EXPECT_EQ(st.code(), StatusCode::kInternal);
}

TEST(GroupCommitWriterTest, ShutdownDrainsPendingEntries) {
    RecordingWalSink sink;
    // Large batch + long timeout: nothing flushes on its own; only Shutdown can.
    auto w = std::make_unique<GroupCommitWriter>(&sink, /*max_batch=*/100, /*timeout_ms=*/100000);

    std::vector<std::future<Status>> futs;
    for (int i = 0; i < 5; ++i) futs.push_back(w->Submit({static_cast<uint8_t>(i)}));

    w->Shutdown();  // must flush the 5 queued entries before returning

    for (auto& f : futs) {
        ASSERT_EQ(f.wait_for(kWait), std::future_status::ready);
        EXPECT_TRUE(f.get().ok());
    }
    EXPECT_EQ(sink.total(), 5u);
}

TEST(GroupCommitWriterTest, SubmitAfterShutdownIsRejected) {
    RecordingWalSink sink;
    GroupCommitWriter w(&sink, /*max_batch=*/4, /*timeout_ms=*/10);
    w.Shutdown();

    auto fut = w.Submit({1});
    ASSERT_EQ(fut.wait_for(kWait), std::future_status::ready);
    Status st = fut.get();
    EXPECT_FALSE(st.ok());
    EXPECT_EQ(st.code(), StatusCode::kUnavailable);
}

TEST(GroupCommitWriterTest, StatsReflectActivity) {
    RecordingWalSink sink;
    GroupCommitWriter w(&sink, /*max_batch=*/2, /*timeout_ms=*/10);

    auto f1 = w.Submit({1});
    auto f2 = w.Submit({2});
    f1.wait_for(kWait);
    f2.wait_for(kWait);
    EXPECT_TRUE(f1.get().ok());
    EXPECT_TRUE(f2.get().ok());

    GroupCommitStats s = w.GetStats();
    EXPECT_EQ(s.submitted, 2u);
    EXPECT_EQ(s.synced, 2u);
    EXPECT_EQ(s.failed, 0u);
    EXPECT_EQ(s.pending, 0u);
    EXPECT_GE(s.batches, 1u);
}

// Note: the kill -9 / crash-recovery DoD item is a property of the IWalSink
// implementation (F25 PWL / F01 WAL), which owns persistence. GroupCommitWriter's
// unit-level durability contract — "an Ok future implies Sync() already
// succeeded for that batch" — is covered by SingleSubmitFlushesAndSyncs +
// WriteFailurePropagatesAndSkipsSync. Crash-recovery integration lives in F25.

}  // namespace
}  // namespace cortrix::store
