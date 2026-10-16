#include <gtest/gtest.h>
#include "cortrix/spc/spc_task_queue.h"
#include <thread>
#include <chrono>

namespace cortrix {
namespace {

static std::shared_ptr<SPCTask> MakeTask(SPCPriority pri, int64_t created_us,
                                          const std::string& source = "test.txt") {
    auto task = std::make_shared<SPCTask>();
    task->priority = pri;
    task->created_at_us = created_us;
    task->source_path = source;
    return task;
}

TEST(SPCTaskQueueTest, PushAndPop) {
    SPCTaskQueue queue(100);
    auto task = MakeTask(SPCPriority::kP1, 1000);
    EXPECT_EQ(queue.Push(task), 0);
    EXPECT_EQ(queue.Size(), 1u);

    auto popped = queue.Pop();
    ASSERT_NE(popped, nullptr);
    EXPECT_EQ(popped->created_at_us, 1000);
    EXPECT_EQ(queue.Size(), 0u);
}

TEST(SPCTaskQueueTest, PriorityOrdering) {
    SPCTaskQueue queue(100);
    queue.Push(MakeTask(SPCPriority::kP2, 100));  // Low priority
    queue.Push(MakeTask(SPCPriority::kP0, 200));  // Highest priority
    queue.Push(MakeTask(SPCPriority::kP1, 300));  // Medium priority

    auto t1 = queue.Pop();
    auto t2 = queue.Pop();
    auto t3 = queue.Pop();

    ASSERT_NE(t1, nullptr);
    ASSERT_NE(t2, nullptr);
    ASSERT_NE(t3, nullptr);
    EXPECT_EQ(t1->priority, SPCPriority::kP0);
    EXPECT_EQ(t2->priority, SPCPriority::kP1);
    EXPECT_EQ(t3->priority, SPCPriority::kP2);
}

TEST(SPCTaskQueueTest, FifoWithinSamePriority) {
    SPCTaskQueue queue(100);
    queue.Push(MakeTask(SPCPriority::kP1, 100));  // Earlier
    queue.Push(MakeTask(SPCPriority::kP1, 200));  // Later

    auto t1 = queue.Pop();
    auto t2 = queue.Pop();

    ASSERT_NE(t1, nullptr);
    ASSERT_NE(t2, nullptr);
    EXPECT_EQ(t1->created_at_us, 100);  // Earlier first
    EXPECT_EQ(t2->created_at_us, 200);
}

TEST(SPCTaskQueueTest, QueueFullReturnsError) {
    SPCTaskQueue queue(2);
    EXPECT_EQ(queue.Push(MakeTask(SPCPriority::kP1, 1)), 0);
    EXPECT_EQ(queue.Push(MakeTask(SPCPriority::kP1, 2)), 0);
    EXPECT_EQ(queue.Push(MakeTask(SPCPriority::kP1, 3)), -1);  // Full
}

TEST(SPCTaskQueueTest, CancelQueuedVersionsByPath) {
    SPCTaskQueue queue(100);
    queue.Push(MakeTask(SPCPriority::kP1, 1, "file_a.txt"));
    queue.Push(MakeTask(SPCPriority::kP1, 2, "file_b.txt"));
    queue.Push(MakeTask(SPCPriority::kP1, 3, "file_a.txt"));

    // These tasks carry no namespace or source type.
    int cancelled = queue.CancelQueuedVersions("", "", "file_a.txt");
    EXPECT_EQ(cancelled, 2);
}

// #178: one queue serves every namespace, so a cancel must match the namespace and the
// producer as well as the path.
TEST(SPCTaskQueueTest, CancelQueuedVersionsMatchesNamespaceTypeAndPath) {
    SPCTaskQueue queue(100);
    auto make = [](const std::string& ns, const std::string& type, const std::string& path,
                   int64_t at) {
        auto t = MakeTask(SPCPriority::kP1, at, path);
        t->namespace_name = ns;
        t->source_type = type;
        return t;
    };
    auto own = make("ns-a", "http_upload", "report.txt", 1);
    auto other_ns = make("ns-b", "http_upload", "report.txt", 2);
    auto other_type = make("ns-a", "watch_dir", "report.txt", 3);
    auto other_path = make("ns-a", "http_upload", "notes.txt", 4);
    for (const auto& t : {own, other_ns, other_type, other_path}) {
        ASSERT_EQ(queue.Push(t), 0);
    }

    EXPECT_EQ(queue.CancelQueuedVersions("ns-a", "http_upload", "report.txt"), 1);
    EXPECT_TRUE(own->cancelled.load());
    EXPECT_FALSE(other_ns->cancelled.load());
    EXPECT_FALSE(other_type->cancelled.load());
    EXPECT_FALSE(other_path->cancelled.load());
}

TEST(SPCTaskQueueTest, StopUnblocksWaiters) {
    SPCTaskQueue queue(100);

    std::shared_ptr<SPCTask> result;
    std::thread waiter([&] {
        result = queue.Pop();
    });

    // Give waiter time to block
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    queue.Stop();
    waiter.join();

    EXPECT_EQ(result, nullptr);
    EXPECT_TRUE(queue.IsStopped());
}

TEST(SPCTaskQueueTest, PushAfterStopFails) {
    SPCTaskQueue queue(100);
    queue.Stop();
    EXPECT_EQ(queue.Push(MakeTask(SPCPriority::kP1, 1)), -1);
}

}  // namespace
}  // namespace cortrix
