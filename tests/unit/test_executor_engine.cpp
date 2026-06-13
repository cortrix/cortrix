#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <thread>
#include <vector>

#include "cortrix/common/executor_engine.h"

namespace cortrix {
namespace {

using namespace std::chrono_literals;

TEST(ExecutorEngineTest, SubmitReturnsResult) {
    ExecutorEngine pool(/*workers=*/2, /*queue_size=*/16);
    auto fut = pool.Submit<int>([] { return 21 * 2; });
    EXPECT_EQ(fut.get(), 42);
}

TEST(ExecutorEngineTest, VoidTaskRuns) {
    ExecutorEngine pool(2, 16);
    std::atomic<int> counter{0};
    auto fut = pool.Submit<void>([&counter] { counter.fetch_add(1); });
    fut.get();
    EXPECT_EQ(counter.load(), 1);
}

TEST(ExecutorEngineTest, ManyTasksAllComplete) {
    ExecutorEngine pool(4, 64);
    std::vector<std::future<int>> futs;
    for (int i = 0; i < 200; ++i) {
        futs.push_back(pool.Submit<int>([i] { return i; }));
    }
    int sum = 0;
    for (auto& f : futs) sum += f.get();
    EXPECT_EQ(sum, 199 * 200 / 2);  // 0 + 1 + ... + 199 = 19900
}

// DoD: stress 8 workers / queue=500. Backpressure flows; every task completes.
TEST(ExecutorEngineTest, StressEightWorkersQueue500) {
    ExecutorEngine pool(/*workers=*/8, /*queue_size=*/500);
    constexpr int kTasks = 2000;
    std::vector<std::future<int>> futs;
    futs.reserve(kTasks);
    for (int i = 0; i < kTasks; ++i) {
        futs.push_back(pool.Submit<int>([i] { return i * 2; }));
    }
    long long sum = 0;
    for (auto& f : futs) sum += f.get();
    long long expected = 0;
    for (int i = 0; i < kTasks; ++i) expected += static_cast<long long>(i) * 2;
    EXPECT_EQ(sum, expected);
}

TEST(ExecutorEngineTest, ActiveWorkersReflectsRunningTask) {
    ExecutorEngine pool(2, 8);
    std::promise<void> release;
    std::shared_future<void> gate = release.get_future().share();
    std::atomic<bool> started{false};

    auto fut = pool.Submit<void>([gate, &started] {
        started.store(true);
        gate.wait();  // hold the worker until released
    });

    while (!started.load()) std::this_thread::sleep_for(1ms);
    EXPECT_EQ(pool.ActiveWorkers(), 1);

    release.set_value();
    fut.get();
    // After completion the worker is idle again.
    std::this_thread::sleep_for(5ms);
    EXPECT_EQ(pool.ActiveWorkers(), 0);
}

TEST(ExecutorEngineTest, QueueDepthReflectsBacklog) {
    ExecutorEngine pool(/*workers=*/1, /*queue_size=*/16);
    std::promise<void> release;
    std::shared_future<void> gate = release.get_future().share();
    std::atomic<bool> started{false};

    // Occupy the single worker.
    auto busy = pool.Submit<void>([gate, &started] {
        started.store(true);
        gate.wait();
    });
    while (!started.load()) std::this_thread::sleep_for(1ms);

    // These three pile up in the queue while the worker is blocked.
    auto a = pool.Submit<int>([] { return 1; });
    auto b = pool.Submit<int>([] { return 2; });
    auto c = pool.Submit<int>([] { return 3; });
    std::this_thread::sleep_for(5ms);
    EXPECT_EQ(pool.QueueDepth(), 3);

    release.set_value();
    EXPECT_EQ(a.get() + b.get() + c.get(), 6);
    busy.get();
}

TEST(ExecutorEngineTest, ShutdownDrainsQueuedTasks) {
    auto pool = std::make_unique<ExecutorEngine>(/*workers=*/1, /*queue_size=*/100);
    std::atomic<int> done{0};
    std::vector<std::future<void>> futs;
    for (int i = 0; i < 10; ++i) {
        futs.push_back(pool->Submit<void>([&done] {
            std::this_thread::sleep_for(1ms);
            done.fetch_add(1);
        }));
    }
    pool->Shutdown();  // must let all 10 finish
    for (auto& f : futs) f.get();
    EXPECT_EQ(done.load(), 10);
}

}  // namespace
}  // namespace cortrix
