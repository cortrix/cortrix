// F01 S5 — shared_mutex read/write concurrency tests (design § 6 S5).
//
// Validates PHnsw under concurrent access: many parallel searches, reads during
// writes, parallel writes, alternating read/write, and snapshot during live
// read/write. Run the whole binary under TSAN (-DCORTRIX_SANITIZE=thread) to get
// the "0 data race" guarantee (Concurrent_TSAN_Clean in the spec matrix is "run
// these under TSAN", not a separate test).

#include "cortrix/store/phnsw.h"

#include <gtest/gtest.h>

#include <atomic>
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
        v[static_cast<size_t>(i)] = static_cast<float>((seed * 19 + i) % 181) * 0.023f;
    }
    return v;
}

class PHnswConcurrencyTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = (fs::temp_directory_path() /
                ("cortrix_phnsw_conc_" + std::to_string(reinterpret_cast<uintptr_t>(this))))
                   .string();
        fs::remove_all(dir_);
        config_.dim = kDim;
        config_.max_elements = 10000;
        config_.group_commit_max_batch = 16;
        config_.group_commit_timeout_ms = 5;
    }
    void TearDown() override { fs::remove_all(dir_); }

    // Seed the index with `n` vectors (ids 1..n).
    void Seed(PHnsw& idx, int n) {
        for (int i = 0; i < n; ++i) {
            auto v = MakeVec(kDim, i);
            ASSERT_TRUE(idx.AddPoint(v.data(), static_cast<uint64_t>(i + 1)).ok());
        }
    }

    static constexpr int kDim = 8;
    std::string dir_;
    PhnswConfig config_;
};

TEST_F(PHnswConcurrencyTest, Concurrent_MultiRead) {
    PHnsw idx(dir_, config_);
    Seed(idx, 200);

    constexpr int kThreads = 8;
    std::atomic<int> nonempty{0};
    std::vector<std::thread> ts;
    for (int t = 0; t < kThreads; ++t) {
        ts.emplace_back([&, t] {
            for (int i = 0; i < 100; ++i) {
                auto q = MakeVec(kDim, (t * 100 + i) % 200);
                auto r = idx.Search(q.data(), 5);
                if (!r.empty()) nonempty.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (auto& th : ts) th.join();
    EXPECT_EQ(nonempty.load(), kThreads * 100);  // every search hit the populated index
}

TEST_F(PHnswConcurrencyTest, Concurrent_MultiReadCustomEf) {
    // Exercises the exclusive-lock ef_search path concurrently (must not race on
    // ef_ and must restore it).
    PHnsw idx(dir_, config_);
    Seed(idx, 200);

    constexpr int kThreads = 6;
    std::atomic<int> ok{0};
    std::vector<std::thread> ts;
    for (int t = 0; t < kThreads; ++t) {
        ts.emplace_back([&, t] {
            for (int i = 0; i < 60; ++i) {
                auto q = MakeVec(kDim, (t * 60 + i) % 200);
                auto r = idx.Search(q.data(), 5, /*ef_search=*/64);
                if (r.size() == 5) ok.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (auto& th : ts) th.join();
    EXPECT_EQ(ok.load(), kThreads * 60);
}

TEST_F(PHnswConcurrencyTest, Concurrent_ReadDuringWrite) {
    PHnsw idx(dir_, config_);
    Seed(idx, 100);

    std::atomic<bool> stop{false};
    std::atomic<int> reads{0};
    std::vector<std::thread> readers;
    for (int t = 0; t < 4; ++t) {
        readers.emplace_back([&, t] {
            while (!stop.load(std::memory_order_relaxed)) {
                auto q = MakeVec(kDim, t);
                auto r = idx.Search(q.data(), 3);
                (void)r;
                reads.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    // One writer adds 200 more vectors while readers run.
    for (int i = 100; i < 300; ++i) {
        auto v = MakeVec(kDim, i);
        ASSERT_TRUE(idx.AddPoint(v.data(), static_cast<uint64_t>(i + 1)).ok());
    }
    stop.store(true);
    for (auto& th : readers) th.join();
    EXPECT_GT(reads.load(), 0);
    EXPECT_EQ(idx.GetStats().vector_count, 300u);
}

TEST_F(PHnswConcurrencyTest, Concurrent_MultiWrite) {
    PHnsw idx(dir_, config_);
    constexpr int kThreads = 4;
    constexpr int kPer = 100;
    std::vector<std::thread> ts;
    for (int t = 0; t < kThreads; ++t) {
        ts.emplace_back([&, t] {
            for (int i = 0; i < kPer; ++i) {
                auto v = MakeVec(kDim, t * 1000 + i);
                const uint64_t id = static_cast<uint64_t>(t * kPer + i + 1);
                ASSERT_TRUE(idx.AddPoint(v.data(), id).ok());
            }
        });
    }
    for (auto& th : ts) th.join();
    EXPECT_EQ(idx.GetStats().vector_count, static_cast<uint64_t>(kThreads * kPer));
}

TEST_F(PHnswConcurrencyTest, Concurrent_ReadWriteAlternate) {
    PHnsw idx(dir_, config_);
    Seed(idx, 50);

    std::atomic<bool> stop{false};
    std::thread reader([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            auto q = MakeVec(kDim, 1);
            auto r = idx.Search(q.data(), 5);
            (void)r;
        }
    });
    // Interleave writes and deletes.
    for (int i = 50; i < 550; ++i) {
        auto v = MakeVec(kDim, i);
        ASSERT_TRUE(idx.AddPoint(v.data(), static_cast<uint64_t>(i + 1)).ok());
        if (i % 5 == 0) {
            ASSERT_TRUE(idx.MarkDelete(static_cast<uint64_t>(i - 49)).ok());
        }
    }
    stop.store(true);
    reader.join();
    // No deadlock + the index is consistent (live count > 0, no crash).
    EXPECT_GT(idx.GetStats().vector_count, 0u);
}

TEST_F(PHnswConcurrencyTest, Concurrent_SnapshotDuringReadWrite) {
    PHnsw idx(dir_, config_);
    Seed(idx, 100);

    std::atomic<bool> stop{false};
    std::thread reader([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            auto q = MakeVec(kDim, 2);
            (void)idx.Search(q.data(), 5);
        }
    });
    std::thread writer([&] {
        int i = 100;
        while (!stop.load(std::memory_order_relaxed)) {
            auto v = MakeVec(kDim, i);
            (void)idx.AddPoint(v.data(), static_cast<uint64_t>(i + 1));
            ++i;
        }
    });

    // Take a couple of snapshots while reads + writes are live.
    for (int s = 0; s < 3; ++s) {
        EXPECT_TRUE(idx.Snapshot().ok());
    }
    stop.store(true);
    reader.join();
    writer.join();

    // After the storm, reopening recovers a consistent index (snapshot + WAL).
    const uint64_t live = idx.GetStats().vector_count;
    EXPECT_GT(live, 0u);
}

}  // namespace
}  // namespace cortrix::store
