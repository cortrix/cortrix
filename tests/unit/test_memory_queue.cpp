#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "cortrix/memory/memory_queue.h"

// S2 coverage: the memory extraction async queue infrastructure (D2) — push/backpressure,
// worker-pool drain through the handler, retry → dead-letter, trace propagation,
// and the periodic-scan fallback. Self-contained C++ queue (no async task dependency).
namespace cortrix::memory {
namespace {

InteractionLog MakeInteraction(const std::string& id, const std::string& content) {
    InteractionLog i;
    i.id = id;
    i.session_id = "s1";
    i.namespace_name = "memory";
    i.user_id = "u1";
    i.role = "user";
    i.content = content;
    return i;
}

// Spin-wait helper (bounded) so the multithreaded tests are not flaky.
template <typename Pred>
bool WaitUntil(Pred pred, int timeout_ms = 2000) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return pred();
}

TEST(MemoryQueueTest, PushIncrementsDepth) {
    MemoryQueue::Config cfg;
    cfg.worker_count = 1;
    MemoryQueue q(cfg);  // no workers started → items accumulate
    EXPECT_EQ(q.Depth(), 0u);
    EXPECT_TRUE(q.Push(MakeInteraction("i1", "hello")));
    EXPECT_TRUE(q.Push(MakeInteraction("i2", "world")));
    EXPECT_EQ(q.Depth(), 2u);
}

TEST(MemoryQueueTest, BackpressureRejectsBeyondMax) {
    MemoryQueue::Config cfg;
    cfg.queue_max_size = 2;
    MemoryQueue q(cfg);
    EXPECT_TRUE(q.Push(MakeInteraction("i1", "a")));
    EXPECT_TRUE(q.Push(MakeInteraction("i2", "b")));
    EXPECT_FALSE(q.Push(MakeInteraction("i3", "c")));  // full
    EXPECT_EQ(q.Depth(), 2u);
}

TEST(MemoryQueueTest, WorkersDrainQueueThroughHandler) {
    MemoryQueue::Config cfg;
    cfg.worker_count = 3;
    MemoryQueue q(cfg);

    std::atomic<int> processed{0};
    std::mutex seen_mu;
    std::vector<std::string> seen;
    q.StartWorkers([&](const MemoryQueueItem& item) {
        {
            std::lock_guard<std::mutex> lk(seen_mu);
            seen.push_back(item.interaction.id);
        }
        processed.fetch_add(1);
        return true;
    });

    for (int i = 0; i < 20; ++i) {
        q.Push(MakeInteraction("i" + std::to_string(i), "c"));
    }
    EXPECT_TRUE(WaitUntil([&] { return processed.load() == 20; }));
    q.Stop();
    EXPECT_EQ(processed.load(), 20);
    EXPECT_EQ(q.processed_count(), 20u);
    EXPECT_EQ(seen.size(), 20u);
    EXPECT_EQ(q.worker_count(), 0);  // joined after Stop
}

TEST(MemoryQueueTest, FailedItemRetriesThenDeadLetters) {
    MemoryQueue::Config cfg;
    cfg.worker_count = 1;
    cfg.retry_max = 2;
    MemoryQueue q(cfg);

    std::atomic<int> attempts{0};
    std::atomic<int> dead{0};
    q.StartWorkers(
        [&](const MemoryQueueItem&) {
            attempts.fetch_add(1);
            return false;  // always fail
        },
        [&](const MemoryQueueItem&) { dead.fetch_add(1); });

    q.Push(MakeInteraction("i1", "always-fails"));
    // 1 initial + retry_max(2) retries = 3 attempts, then 1 dead-letter.
    EXPECT_TRUE(WaitUntil([&] { return dead.load() == 1; }));
    q.Stop();
    EXPECT_EQ(attempts.load(), 3);
    EXPECT_EQ(dead.load(), 1);
    EXPECT_EQ(q.dead_letter_count(), 1u);
}

TEST(MemoryQueueTest, TracePropagatesToHandler) {
    MemoryQueue q(MemoryQueue::Config{});
    std::atomic<bool> got_ctx{false};
    std::string seen_trace;
    std::mutex mu;
    q.StartWorkers([&](const MemoryQueueItem& item) {
        std::lock_guard<std::mutex> lk(mu);
        got_ctx.store(item.has_ctx);
        if (item.has_ctx) seen_trace = item.ctx.trace_id;
        return true;
    });

    observability::TraceContext ctx;
    ctx.trace_id = "abc123";
    ctx.span_id = "def456";
    q.Push(MakeInteraction("i1", "traced"), &ctx);

    EXPECT_TRUE(WaitUntil([&] { return q.processed_count() == 1u; }));
    q.Stop();
    EXPECT_TRUE(got_ctx.load());
    EXPECT_EQ(seen_trace, "abc123");
}

TEST(MemoryQueueTest, RunPeriodicScanOnceEnqueuesUnextracted) {
    MemoryQueue q(MemoryQueue::Config{});  // no workers → items stay queued
    auto source = [] {
        return std::vector<InteractionLog>{MakeInteraction("u1", "a"),
                                           MakeInteraction("u2", "b")};
    };
    int n = q.RunPeriodicScanOnce(source);
    EXPECT_EQ(n, 2);
    EXPECT_EQ(q.Depth(), 2u);
}

TEST(MemoryQueueTest, PeriodicScanReenqueuesAndWorkersProcess) {
    MemoryQueue::Config cfg;
    cfg.worker_count = 2;
    cfg.periodic_scan_interval_s = 3600;  // long; we drive RunPeriodicScanOnce manually
    MemoryQueue q(cfg);

    std::atomic<int> processed{0};
    q.StartWorkers([&](const MemoryQueueItem&) {
        processed.fetch_add(1);
        return true;
    });

    auto source = [] {
        return std::vector<InteractionLog>{MakeInteraction("p1", "x"),
                                           MakeInteraction("p2", "y"),
                                           MakeInteraction("p3", "z")};
    };
    EXPECT_EQ(q.RunPeriodicScanOnce(source), 3);
    EXPECT_TRUE(WaitUntil([&] { return processed.load() == 3; }));
    q.Stop();
    EXPECT_EQ(processed.load(), 3);
}

TEST(MemoryQueueTest, DefaultConstructorUsesDefaults) {
    MemoryQueue q;  // default Config
    q.StartWorkers([](const MemoryQueueItem&) { return true; });
    EXPECT_GE(q.worker_count(), 1);
    q.Push(MakeInteraction("i1", "x"));
    EXPECT_TRUE(WaitUntil([&] { return q.processed_count() == 1u; }));
    q.Stop();
}

TEST(MemoryQueueTest, ThreadedPeriodicScanReenqueues) {
    MemoryQueue::Config cfg;
    cfg.worker_count = 1;
    cfg.periodic_scan_interval_s = 0;  // fire immediately + repeatedly
    MemoryQueue q(cfg);

    std::atomic<int> processed{0};
    q.StartWorkers([&](const MemoryQueueItem&) {
        processed.fetch_add(1);
        return true;
    });

    std::atomic<bool> emitted{false};
    // The periodic source yields one batch on its first call, then nothing —
    // so the worker drains a known count regardless of how many ticks fire.
    q.StartPeriodicScan([&]() -> std::vector<InteractionLog> {
        if (emitted.exchange(true)) return {};
        return {MakeInteraction("p1", "x"), MakeInteraction("p2", "y")};
    });

    EXPECT_TRUE(WaitUntil([&] { return processed.load() >= 2; }, 3000));
    q.Stop();
    EXPECT_GE(processed.load(), 2);
}

TEST(MemoryQueueTest, StartPeriodicScanIsNoOpWhenAlreadyStarted) {
    MemoryQueue::Config cfg;
    cfg.periodic_scan_interval_s = 3600;
    MemoryQueue q(cfg);
    q.StartPeriodicScan([] { return std::vector<InteractionLog>{}; });
    q.StartPeriodicScan([] { return std::vector<InteractionLog>{}; });  // ignored
    q.Stop();  // no hang / no double-join
    SUCCEED();
}

TEST(MemoryQueueTest, RunPeriodicScanOnceWithNullSourceIsZero) {
    MemoryQueue q;
    EXPECT_EQ(q.RunPeriodicScanOnce(nullptr), 0);
    q.Stop();
}

TEST(MemoryQueueTest, StopIsIdempotent) {
    MemoryQueue q(MemoryQueue::Config{});
    q.StartWorkers([](const MemoryQueueItem&) { return true; });
    q.Stop();
    q.Stop();  // no crash / no hang
    EXPECT_EQ(q.worker_count(), 0);
}

TEST(MemoryQueueTest, StartWorkersIsNoOpWhenAlreadyStarted) {
    MemoryQueue::Config cfg;
    cfg.worker_count = 2;
    MemoryQueue q(cfg);
    q.StartWorkers([](const MemoryQueueItem&) { return true; });
    q.StartWorkers([](const MemoryQueueItem&) { return false; });  // ignored
    EXPECT_EQ(q.worker_count(), 2);
    q.Stop();
}

}  // namespace
}  // namespace cortrix::memory
