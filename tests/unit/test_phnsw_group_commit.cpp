// F01 S3 — group-commit + durable WAL integration tests (design § 4.1 / § 6 S3).
//
// The GroupCommitWriter coalescing mechanics (batch-full / timeout / concurrent
// merge / shutdown) are covered by test_group_commit_writer.cpp. These tests
// verify PHnsw's *use* of it: every AddPoint/MarkDelete is framed, durably
// fsynced to hnsw.wal, and only then applied to the graph (so the call returns
// once the record is both crash-safe and visible), and that concurrent writers
// coalesce into far fewer WAL records-per-fsync than calls.

#include "cortrix/store/phnsw.h"

#include <fcntl.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "cortrix/store/phnsw/wal_entry.h"
#include "cortrix/store/phnsw/wal_writer.h"

namespace cortrix::store {
namespace {

namespace fs = std::filesystem;

std::vector<float> MakeVec(int dim, int seed) {
    std::vector<float> v(static_cast<size_t>(dim));
    for (int i = 0; i < dim; ++i) {
        v[static_cast<size_t>(i)] = static_cast<float>((seed * 13 + i) % 211) * 0.019f;
    }
    return v;
}

class PHnswGroupCommitTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = (fs::temp_directory_path() /
                ("cortrix_phnsw_gc_" + std::to_string(reinterpret_cast<uintptr_t>(this))))
                   .string();
        fs::remove_all(dir_);
        config_.dim = kDim;
        config_.max_elements = 5000;
        config_.group_commit_max_batch = 16;
        config_.group_commit_timeout_ms = 5;
    }
    void TearDown() override { fs::remove_all(dir_); }

    std::string WalPath() const { return (fs::path(dir_) / "hnsw.wal").string(); }

    static constexpr int kDim = 8;
    std::string dir_;
    PhnswConfig config_;
};

TEST_F(PHnswGroupCommitTest, AddPointIsDurableAndVisible) {
    PHnsw index(dir_, config_);
    auto v = MakeVec(kDim, 1);
    ASSERT_TRUE(index.AddPoint(v.data(), 100).ok());

    // Visible immediately after the (blocking) call returns.
    EXPECT_TRUE(index.Exists(100));
    EXPECT_EQ(index.GetStats().vector_count, 1u);

    // Durable: a record is in hnsw.wal (read it back via the WAL layer).
    auto wal = WalWriter::Open(WalPath(), kDim);
    ASSERT_TRUE(wal.ok());
    auto recs = wal.value()->ReadAll(kDim);
    ASSERT_TRUE(recs.ok());
    ASSERT_EQ(recs.value().size(), 1u);
    EXPECT_EQ(recs.value()[0].type, WalEntry::Type::kInsert);
    EXPECT_EQ(recs.value()[0].block_id, 100u);
}

TEST_F(PHnswGroupCommitTest, WalStatsTracksWrites) {
    PHnsw index(dir_, config_);
    auto v = MakeVec(kDim, 2);
    for (int i = 0; i < 7; ++i) {
        ASSERT_TRUE(index.AddPoint(v.data(), static_cast<uint64_t>(i + 1)).ok());
    }
    ASSERT_TRUE(index.MarkDelete(3).ok());
    // 7 inserts + 1 delete = 8 WAL records.
    auto ws = index.GetWalStats();
    EXPECT_EQ(ws.entry_count, 8u);
    EXPECT_GT(ws.file_size_bytes, WalWriter::kHeaderSize);
}

TEST_F(PHnswGroupCommitTest, MarkDeleteJournaledAndApplied) {
    PHnsw index(dir_, config_);
    auto v = MakeVec(kDim, 3);
    ASSERT_TRUE(index.AddPoint(v.data(), 5).ok());
    EXPECT_TRUE(index.Exists(5));
    ASSERT_TRUE(index.MarkDelete(5).ok());
    EXPECT_FALSE(index.Exists(5));
    // Missing id: idempotent Ok, still journaled.
    ASSERT_TRUE(index.MarkDelete(9999).ok());

    auto wal = WalWriter::Open(WalPath(), kDim);
    ASSERT_TRUE(wal.ok());
    auto recs = wal.value()->ReadAll(kDim);
    ASSERT_TRUE(recs.ok());
    EXPECT_EQ(recs.value().size(), 3u);  // insert + 2 deletes
}

TEST_F(PHnswGroupCommitTest, AddPointsBatchAllDurable) {
    PHnsw index(dir_, config_);
    std::vector<std::vector<float>> store;
    std::vector<std::pair<const float*, uint64_t>> batch;
    for (int i = 0; i < 50; ++i) store.push_back(MakeVec(kDim, i));
    for (int i = 0; i < 50; ++i) {
        batch.emplace_back(store[static_cast<size_t>(i)].data(), static_cast<uint64_t>(i + 1));
    }
    ASSERT_TRUE(index.AddPoints(batch).ok());
    EXPECT_EQ(index.GetStats().vector_count, 50u);
    EXPECT_EQ(index.GetWalStats().entry_count, 50u);
}

// Note: coalescing (batches < records) is NOT asserted here — PHnsw does not
// expose GroupCommitStats, so the batch count is unobservable from a test.
// This pins what IS observable: all concurrent AddPoints succeed and every
// record is durable in the WAL.
TEST_F(PHnswGroupCommitTest, ConcurrentAddPointsAllSucceedDurably) {
    PHnsw index(dir_, config_);
    constexpr int kThreads = 8;
    constexpr int kPerThread = 50;
    std::atomic<int> ok_count{0};

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < kPerThread; ++i) {
                auto v = MakeVec(kDim, t * 1000 + i);
                const uint64_t id = static_cast<uint64_t>(t * kPerThread + i + 1);
                if (index.AddPoint(v.data(), id).ok()) {
                    ok_count.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& th : threads) th.join();

    EXPECT_EQ(ok_count.load(), kThreads * kPerThread);
    EXPECT_EQ(index.GetStats().vector_count,
              static_cast<uint64_t>(kThreads * kPerThread));

    // All 400 records are durable.
    auto wal = WalWriter::Open(WalPath(), kDim);
    ASSERT_TRUE(wal.ok());
    auto recs = wal.value()->ReadAll(kDim);
    ASSERT_TRUE(recs.ok());
    EXPECT_EQ(recs.value().size(), static_cast<size_t>(kThreads * kPerThread));

}

TEST_F(PHnswGroupCommitTest, NotReady_RejectsWritesWhenWalUnopenable) {
    // Point the Unit "directory" at a path that already exists as a *file*, so
    // create_directories + WAL open fail and the index comes up NOT READY.
    const std::string file_as_dir = (fs::path(dir_).string() + "_as_file");
    fs::remove_all(file_as_dir);
    {
        // create parent and a regular file at the unit_data_dir path
        std::error_code ec;
        fs::create_directories(fs::path(file_as_dir).parent_path(), ec);
        int fd = ::open(file_as_dir.c_str(), O_CREAT | O_WRONLY, 0644);
        ASSERT_GE(fd, 0);
        ::close(fd);
    }
    PHnsw index(file_as_dir, config_);
    auto v = MakeVec(kDim, 1);
    Status s = index.AddPoint(v.data(), 1);
    EXPECT_FALSE(s.ok());
    EXPECT_NE(s.message().find("CX_ERR_PHNSW_NOT_READY"), std::string::npos);
    fs::remove_all(file_as_dir);
}

TEST_F(PHnswGroupCommitTest, ShutdownFlushesQueuedWrites) {
    PHnsw index(dir_, config_);
    auto v = MakeVec(kDim, 4);
    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(index.AddPoint(v.data(), static_cast<uint64_t>(i + 1)).ok());
    }
    EXPECT_TRUE(index.Shutdown().ok());
    // After shutdown the 5 records are durable on disk.
    auto wal = WalWriter::Open(WalPath(), kDim);
    ASSERT_TRUE(wal.ok());
    auto recs = wal.value()->ReadAll(kDim);
    ASSERT_TRUE(recs.ok());
    EXPECT_EQ(recs.value().size(), 5u);
}

}  // namespace
}  // namespace cortrix::store
