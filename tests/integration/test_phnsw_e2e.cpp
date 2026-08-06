// Index S6 — P-HNSW end-to-end integration (index-internal, design S6).
//
// A cohesive durable-lifecycle scenario exercised purely at the PHnsw level (the
// cross-Feature SPC/Query-path E2E lives in integration once the consumer migration
// lands): bulk write -> search hits -> simulated crash (drop the object without
// Shutdown) -> reopen recovers -> more writes trip an auto-snapshot -> reopen
// again and everything is still searchable.

#include "cortrix/store/phnsw.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace cortrix::store {
namespace {

namespace fs = std::filesystem;

std::vector<float> RandomVec(int dim, std::mt19937& rng) {
    std::uniform_real_distribution<float> d(-1.0f, 1.0f);
    std::vector<float> v(static_cast<size_t>(dim));
    for (auto& x : v) x = d(rng);
    return v;
}

bool WaitForWalBelow(PHnsw& idx, uint64_t below, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (idx.GetWalStats().entry_count < below) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return idx.GetWalStats().entry_count < below;
}

class PHnswE2ETest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = (fs::temp_directory_path() /
                ("cortrix_phnsw_e2e_" + std::to_string(reinterpret_cast<uintptr_t>(this))))
                   .string();
        fs::remove_all(dir_);
    }
    void TearDown() override { fs::remove_all(dir_); }

    PhnswConfig Config() {
        PhnswConfig c;
        c.dim = kDim;
        c.max_elements = 20000;
        c.snapshot_max_wal_entries = 500;  // low so the E2E trips auto-snapshot
        c.snapshot_max_wal_size_mb = 0;
        return c;
    }

    static constexpr int kDim = 64;
    std::string dir_;
};

TEST_F(PHnswE2ETest, WriteSearchCrashRecoverAutoSnapshot) {
    std::mt19937 rng(42);
    // Keep the first vector to use as a deterministic query whose nearest
    // neighbor is its own id.
    std::vector<float> first;

    // --- Phase 1: bulk write, then a simulated crash (no Shutdown). ---
    {
        PHnsw idx(dir_, Config());
        for (int i = 0; i < 300; ++i) {
            auto v = RandomVec(kDim, rng);
            if (i == 0) first = v;
            ASSERT_TRUE(idx.AddPoint(v.data(), static_cast<uint64_t>(i + 1)).ok());
        }
        // Search finds the populated data.
        auto r = idx.Search(first.data(), 1);
        ASSERT_EQ(r.size(), 1u);
        EXPECT_EQ(r[0].first, 1u);
        EXPECT_EQ(idx.GetStats().vector_count, 300u);
        // Drop the object WITHOUT Shutdown() — simulates a crash. The 300 records
        // are durable in hnsw.wal (each AddPoint fsynced), recoverable on reopen.
    }

    // --- Phase 2: reopen recovers from the WAL (no snapshot yet at 300 < 500). ---
    {
        PHnsw idx(dir_, Config());
        EXPECT_EQ(idx.GetStats().vector_count, 300u);
        auto r = idx.Search(first.data(), 1);
        ASSERT_EQ(r.size(), 1u);
        EXPECT_EQ(r[0].first, 1u);

        // --- Phase 3: more writes cross the 500-entry threshold -> auto-snapshot. ---
        for (int i = 300; i < 700; ++i) {
            auto v = RandomVec(kDim, rng);
            ASSERT_TRUE(idx.AddPoint(v.data(), static_cast<uint64_t>(i + 1)).ok());
        }
        EXPECT_TRUE(WaitForWalBelow(idx, /*below=*/500, std::chrono::milliseconds(4000)));
        EXPECT_EQ(idx.GetStats().vector_count, 700u);
        idx.Shutdown();  // clean close this time
    }

    // --- Phase 4: reopen after auto-snapshot — snapshot + residual WAL. ---
    {
        PHnsw idx(dir_, Config());
        EXPECT_EQ(idx.GetStats().vector_count, 700u);
        EXPECT_TRUE(idx.Exists(1));     // from the first batch (now in a snapshot)
        EXPECT_TRUE(idx.Exists(700));   // from the post-snapshot WAL
        auto r = idx.Search(first.data(), 1);
        ASSERT_EQ(r.size(), 1u);
        EXPECT_EQ(r[0].first, 1u);
    }
}

TEST_F(PHnswE2ETest, DeleteThenRecover) {
    std::mt19937 rng(7);
    std::vector<std::vector<float>> vecs;
    {
        PHnsw idx(dir_, Config());
        for (int i = 0; i < 100; ++i) {
            auto v = RandomVec(kDim, rng);
            vecs.push_back(v);
            ASSERT_TRUE(idx.AddPoint(v.data(), static_cast<uint64_t>(i + 1)).ok());
        }
        ASSERT_TRUE(idx.MarkDelete(50).ok());
        ASSERT_TRUE(idx.MarkDelete(51).ok());
        idx.Shutdown();
    }
    PHnsw idx(dir_, Config());
    EXPECT_EQ(idx.GetStats().vector_count, 98u);
    EXPECT_FALSE(idx.Exists(50));
    EXPECT_FALSE(idx.Exists(51));
    EXPECT_TRUE(idx.Exists(1));
    // A deleted id must not appear in search results.
    auto r = idx.Search(vecs[49].data(), 5);
    for (const auto& [id, dist] : r) {
        EXPECT_NE(id, 50u);
        EXPECT_NE(id, 51u);
        (void)dist;
    }
}

}  // namespace
}  // namespace cortrix::store
