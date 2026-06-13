// S2.1 — RerankerThreadPool: 4 resident workers, Submit/Wait, concurrent safety,
// graceful shutdown, SubmitWaitFor timeout.
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "cortrix/reranker/reranker_thread_pool.h"

namespace cortrix::reranker {
namespace {

TEST(RerankerThreadPoolTest, CreatesRequestedWorkerCount) {
    RerankerThreadPool<float> pool(4, 200, 5000);
    EXPECT_EQ(pool.num_workers(), 4);
    EXPECT_EQ(pool.task_timeout_ms(), 5000);
    EXPECT_EQ(pool.queue_capacity(), 200u);
}

TEST(RerankerThreadPoolTest, ClampsDegenerateParams) {
    RerankerThreadPool<float> pool(0, 0, 1000);
    EXPECT_GE(pool.num_workers(), 1);          // workers clamped to >= 1
    EXPECT_GE(pool.queue_capacity(), 1u);      // queue clamped to >= 1
}

TEST(RerankerThreadPoolTest, SubmitRunsTaskAndReturnsValue) {
    RerankerThreadPool<float> pool(4, 200, 5000);
    auto fut = pool.Submit([] { return 0.5f; });
    EXPECT_FLOAT_EQ(fut.get(), 0.5f);
}

TEST(RerankerThreadPoolTest, ManyTasksAllExecuteOnce) {
    RerankerThreadPool<float> pool(4, 200, 5000);
    std::atomic<int> counter{0};
    std::vector<std::future<float>> futures;
    constexpr int kN = 200;
    futures.reserve(kN);
    for (int i = 0; i < kN; ++i) {
        futures.push_back(pool.Submit([&counter] {
            counter.fetch_add(1, std::memory_order_relaxed);
            return 1.0f;
        }));
    }
    for (auto& f : futures) EXPECT_FLOAT_EQ(f.get(), 1.0f);
    EXPECT_EQ(counter.load(), kN);
}

TEST(RerankerThreadPoolTest, ConcurrentSubmittersAreSafe) {
    RerankerThreadPool<float> pool(4, 50, 5000);
    std::atomic<int> done{0};
    std::vector<std::thread> submitters;
    constexpr int kThreads = 8;
    constexpr int kPer = 50;
    for (int t = 0; t < kThreads; ++t) {
        submitters.emplace_back([&] {
            std::vector<std::future<float>> fs;
            for (int i = 0; i < kPer; ++i) {
                fs.push_back(pool.Submit([&done] {
                    done.fetch_add(1, std::memory_order_relaxed);
                    return 2.0f;
                }));
            }
            for (auto& f : fs) f.get();
        });
    }
    for (auto& s : submitters) s.join();
    EXPECT_EQ(done.load(), kThreads * kPer);
}

TEST(RerankerThreadPoolTest, SubmitWaitForReturnsValueWithinBudget) {
    RerankerThreadPool<float> pool(4, 200, 5000);
    auto [val, ok] = pool.SubmitWaitFor([] { return 0.9f; });
    EXPECT_TRUE(ok);
    EXPECT_FLOAT_EQ(val, 0.9f);
}

TEST(RerankerThreadPoolTest, SubmitWaitForTimesOutReturnsFalse) {
    RerankerThreadPool<float> pool(1, 200, /*task_timeout_ms=*/20);
    // Task sleeps well past the 20ms budget → SubmitWaitFor reports timeout.
    auto [val, ok] = pool.SubmitWaitFor([] {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        return 1.0f;
    });
    EXPECT_FALSE(ok);
    EXPECT_FLOAT_EQ(val, 0.0f);  // caller maps timeout → score=0 (S3.4)
}

TEST(RerankerThreadPoolTest, ShutdownIsIdempotentAndUnblocks) {
    RerankerThreadPool<float> pool(2, 200, 5000);
    pool.Submit([] { return 1.0f; }).get();
    pool.Shutdown();
    pool.Shutdown();  // idempotent, no crash/hang
    EXPECT_EQ(pool.num_workers(), 0);
}

}  // namespace
}  // namespace cortrix::reranker
