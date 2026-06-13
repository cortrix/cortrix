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

TEST(SPCTaskQueueTest, CancelBySourcePath) {
    SPCTaskQueue queue(100);
    queue.Push(MakeTask(SPCPriority::kP1, 1, "file_a.txt"));
    queue.Push(MakeTask(SPCPriority::kP1, 2, "file_b.txt"));
    queue.Push(MakeTask(SPCPriority::kP1, 3, "file_a.txt"));

    int cancelled = queue.CancelBySourcePath("file_a.txt");
    EXPECT_EQ(cancelled, 2);
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
