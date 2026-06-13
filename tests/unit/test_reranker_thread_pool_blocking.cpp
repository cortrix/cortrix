// S2.3 — blocking policy + queue-full handling (FIFO order, no dropped requests).
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

#include "cortrix/reranker/reranker_thread_pool.h"

namespace cortrix::reranker {
namespace {

using namespace std::chrono_literals;

// A manual gate the test uses to pin worker threads so the queue can be filled
// deterministically.
struct Gate {
    std::mutex m;
    std::condition_variable cv;
    bool open = false;
    void Wait() {
        std::unique_lock<std::mutex> lk(m);
        cv.wait(lk, [this] { return open; });
    }
    void Open() {
        { std::lock_guard<std::mutex> lk(m); open = true; }
        cv.notify_all();
    }
};

TEST(RerankerThreadPoolBlockingTest, SubmitBlocksWhenQueueFullThenUnblocks) {
    // 1 worker, queue capacity 1. Pin the worker, fill the 1 queue slot, then the
    // next Submit must block until the worker is released.
    RerankerThreadPool pool(1, /*queue_size=*/1, 5000);
    Gate gate;

    // Occupy the single worker.
    auto worker_busy = pool.Submit([&gate] { gate.Wait(); return 1.0f; });
    // Give the worker a moment to pick up the task (queue should drain to 0).
    std::this_thread::sleep_for(30ms);

    // Fill the 1 queue slot (this returns immediately — slot was free).
    auto queued = pool.Submit([] { return 2.0f; });

    // The next Submit must BLOCK (worker busy + queue full). Run it on a side
    // thread and assert it does not complete while the gate is shut.
    std::atomic<bool> third_submitted{false};
    std::thread blocker([&] {
        auto f = pool.Submit([] { return 3.0f; });
        third_submitted.store(true);
        f.get();
    });

    std::this_thread::sleep_for(80ms);
    EXPECT_FALSE(third_submitted.load());  // still blocked on the full queue

    // Release the worker → tasks drain → the blocked Submit proceeds.
    gate.Open();
    blocker.join();
    EXPECT_TRUE(third_submitted.load());
    EXPECT_FLOAT_EQ(worker_busy.get(), 1.0f);
    EXPECT_FLOAT_EQ(queued.get(), 2.0f);
}

TEST(RerankerThreadPoolBlockingTest, FifoExecutionOrderWithSingleWorker) {
    // 1 worker → tasks execute strictly in submission (FIFO) order.
    RerankerThreadPool pool(1, /*queue_size=*/100, 5000);
    Gate gate;
    // Pin the worker until all tasks are queued, so queue order == submit order.
    auto pin = pool.Submit([&gate] { gate.Wait(); return 0.0f; });
    std::this_thread::sleep_for(20ms);

    std::vector<int> order;
    std::mutex om;
    std::vector<std::future<float>> fs;
    for (int i = 0; i < 20; ++i) {
        fs.push_back(pool.Submit([i, &order, &om] {
            std::lock_guard<std::mutex> lk(om);
            order.push_back(i);
            return static_cast<float>(i);
        }));
    }
    gate.Open();
    for (auto& f : fs) f.get();
    pin.get();

    ASSERT_EQ(order.size(), 20u);
    for (int i = 0; i < 20; ++i) EXPECT_EQ(order[i], i);  // strict FIFO
}

TEST(RerankerThreadPoolBlockingTest, NoRequestsDroppedUnderBackpressure) {
    // Tiny queue + many submitters → every task must still run exactly once
    // (blocking, not dropping).
    RerankerThreadPool pool(2, /*queue_size=*/2, 5000);
    std::atomic<int> ran{0};
    constexpr int kTotal = 300;
    std::vector<std::future<float>> fs;
    fs.reserve(kTotal);
    for (int i = 0; i < kTotal; ++i) {
        fs.push_back(pool.Submit([&ran] {
            ran.fetch_add(1, std::memory_order_relaxed);
            return 1.0f;
        }));
    }
    for (auto& f : fs) f.get();
    EXPECT_EQ(ran.load(), kTotal);
}

TEST(RerankerThreadPoolBlockingTest, QueueDepthNeverExceedsCapacity) {
    RerankerThreadPool pool(2, /*queue_size=*/4, 5000);
    Gate gate;
    // Pin both workers so submitted tasks accumulate in the queue.
    auto p1 = pool.Submit([&gate] { gate.Wait(); return 0.0f; });
    auto p2 = pool.Submit([&gate] { gate.Wait(); return 0.0f; });
    std::this_thread::sleep_for(20ms);

    // Fill the queue to capacity on a side thread (the 5th would block).
    std::vector<std::future<float>> fs;
    std::thread filler([&] {
        for (int i = 0; i < 4; ++i) fs.push_back(pool.Submit([] { return 1.0f; }));
    });
    filler.join();
    std::this_thread::sleep_for(20ms);
    EXPECT_LE(pool.QueueDepth(), 4);  // capacity is the hard ceiling

    gate.Open();
    p1.get();
    p2.get();
    for (auto& f : fs) f.get();
}

}  // namespace
}  // namespace cortrix::reranker
