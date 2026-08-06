// Index S6 — automatic snapshot triggering (design step 6 / S4
// AutoSnapshot_*). When the WAL grows past snapshot_max_wal_entries or
// snapshot_max_wal_size_mb, a background thread snapshots and truncates the WAL —
// off the write path. These tests drive writes past a low threshold and wait for
// the WAL to be truncated (the observable effect of an auto-snapshot), then
// confirm a snapshot exists and the data survives a reopen.

#include "cortrix/store/phnsw.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace cortrix::store {
namespace {

namespace fs = std::filesystem;

std::vector<float> MakeVec(int dim, int seed) {
    std::vector<float> v(static_cast<size_t>(dim));
    for (int i = 0; i < dim; ++i) {
        v[static_cast<size_t>(i)] = static_cast<float>((seed * 23 + i) % 167) * 0.027f;
    }
    return v;
}

// Poll up to `timeout` for the WAL entry_count to drop below `below` — the
// signature of an auto-snapshot (Snapshot truncates the WAL). Returns true if it
// happened in time.
bool WaitForWalTruncation(PHnsw& idx, uint64_t below, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (idx.GetWalStats().entry_count < below) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return idx.GetWalStats().entry_count < below;
}

bool HasSnapshotFile(const std::string& dir) {
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        const std::string n = e.path().filename().string();
        if (n.rfind("hnsw_", 0) == 0 && n.size() >= 11 &&
            n.compare(n.size() - 11, 11, ".index.meta") == 0) {
            return true;
        }
    }
    return false;
}

class PHnswAutoSnapshotTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = (fs::temp_directory_path() /
                ("cortrix_phnsw_autosnap_" + std::to_string(reinterpret_cast<uintptr_t>(this))))
                   .string();
        fs::remove_all(dir_);
        config_.dim = kDim;
        config_.max_elements = 5000;
    }
    void TearDown() override { fs::remove_all(dir_); }

    static constexpr int kDim = 8;
    std::string dir_;
    PhnswConfig config_;
};

TEST_F(PHnswAutoSnapshotTest, AutoSnapshot_EntryCountThreshold) {
    // Trip the entry-count threshold: snapshot after 20 WAL records.
    config_.snapshot_max_wal_entries = 20;
    config_.snapshot_max_wal_size_mb = 0;  // disable the size trigger
    PHnsw idx(dir_, config_);

    for (int i = 0; i < 50; ++i) {
        auto v = MakeVec(kDim, i);
        ASSERT_TRUE(idx.AddPoint(v.data(), static_cast<uint64_t>(i + 1)).ok());
    }
    // The background thread should snapshot + truncate the WAL below the
    // threshold within a short window.
    EXPECT_TRUE(WaitForWalTruncation(idx, /*below=*/20, std::chrono::milliseconds(3000)));
    EXPECT_TRUE(HasSnapshotFile(dir_));

    // All 50 vectors remain searchable after the auto-snapshot.
    EXPECT_EQ(idx.GetStats().vector_count, 50u);
    EXPECT_TRUE(idx.Exists(1));
    EXPECT_TRUE(idx.Exists(50));
}

TEST_F(PHnswAutoSnapshotTest, AutoSnapshot_SizeThreshold) {
    // Exercise the size branch: a 1 MB WAL floor with the entry trigger disabled.
    // dim=256 makes each INSERT frame ~1 KB, so ~1000 records cross 1 MB.
    PhnswConfig big = config_;
    big.dim = 256;
    big.snapshot_max_wal_entries = 0;    // disable the entry trigger
    big.snapshot_max_wal_size_mb = 1;
    PHnsw idx(dir_, big);

    auto v = MakeVec(256, 1);  // same payload per record; only the label differs
    for (int i = 0; i < 1200; ++i) {
        ASSERT_TRUE(idx.AddPoint(v.data(), static_cast<uint64_t>(i + 1)).ok());
    }
    // After crossing ~1 MB the background snapshot truncates the WAL well below
    // its pre-snapshot record count.
    EXPECT_TRUE(WaitForWalTruncation(idx, /*below=*/1200, std::chrono::milliseconds(5000)));
    EXPECT_TRUE(HasSnapshotFile(dir_));
    EXPECT_EQ(idx.GetStats().vector_count, 1200u);
}

TEST_F(PHnswAutoSnapshotTest, AutoSnapshot_SurvivesReopen) {
    config_.snapshot_max_wal_entries = 15;
    config_.snapshot_max_wal_size_mb = 0;
    {
        PHnsw idx(dir_, config_);
        for (int i = 0; i < 40; ++i) {
            auto v = MakeVec(kDim, i);
            ASSERT_TRUE(idx.AddPoint(v.data(), static_cast<uint64_t>(i + 1)).ok());
        }
        ASSERT_TRUE(WaitForWalTruncation(idx, 15, std::chrono::milliseconds(3000)));
    }
    // Reopen: recovery loads the auto-snapshot + replays the residual WAL.
    PHnsw reopened(dir_, config_);
    EXPECT_EQ(reopened.GetStats().vector_count, 40u);
    EXPECT_TRUE(reopened.Exists(1));
    EXPECT_TRUE(reopened.Exists(40));
}

TEST_F(PHnswAutoSnapshotTest, NoAutoSnapshot_BelowThreshold) {
    // High thresholds -> no auto-snapshot; the WAL keeps every record.
    config_.snapshot_max_wal_entries = 100000;
    config_.snapshot_max_wal_size_mb = 1024;
    PHnsw idx(dir_, config_);
    for (int i = 0; i < 30; ++i) {
        auto v = MakeVec(kDim, i);
        ASSERT_TRUE(idx.AddPoint(v.data(), static_cast<uint64_t>(i + 1)).ok());
    }
    // Give the background thread a moment; it must NOT have snapshotted.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_EQ(idx.GetWalStats().entry_count, 30u);
    EXPECT_FALSE(HasSnapshotFile(dir_));
}

}  // namespace
}  // namespace cortrix::store
