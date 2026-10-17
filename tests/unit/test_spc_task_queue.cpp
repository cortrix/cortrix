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


// #157: an upload of unchanged content is skipped only while that version will still
// be processed, so the queue answers whether a live task waits for a document.
TEST(SPCTaskQueueTest, HasQueuedTaskSeesOnlyLiveWaitingTasks) {
    SPCTaskQueue queue(10);
    auto a = MakeTask(SPCPriority::kP1, 100, "a.txt");
    a->doc_id = "doc-a";
    auto b = MakeTask(SPCPriority::kP1, 200, "b.txt");
    b->doc_id = "doc-b";
    ASSERT_EQ(queue.Push(a), 0);
    ASSERT_EQ(queue.Push(b), 0);
    EXPECT_TRUE(queue.HasQueuedTask("doc-a"));
    EXPECT_TRUE(queue.HasQueuedTask("doc-b"));
    EXPECT_FALSE(queue.HasQueuedTask("doc-c"));

    ASSERT_EQ(queue.CancelQueuedVersions("", "", "b.txt"), 1);
    EXPECT_FALSE(queue.HasQueuedTask("doc-b"));  // cancelled

    auto popped = queue.Pop();
    ASSERT_NE(popped, nullptr);
    EXPECT_EQ(popped->doc_id, "doc-a");
    EXPECT_FALSE(queue.HasQueuedTask("doc-a"));  // a worker took it
}

// #157: a task counts as running from the moment it leaves the queue until it is
// finished, so a document is never seen as neither queued nor running in between.
TEST(SPCTaskQueueTest, APoppedTaskRunsUntilItIsFinished) {
    SPCTaskQueue queue(10);
    auto a = MakeTask(SPCPriority::kP1, 100, "a.txt");
    a->doc_id = "doc-a";
    ASSERT_EQ(queue.Push(a), 0);
    EXPECT_FALSE(queue.IsRunning("doc-a"));

    ASSERT_NE(queue.Pop(), nullptr);
    EXPECT_FALSE(queue.HasQueuedTask("doc-a"));
    EXPECT_TRUE(queue.IsRunning("doc-a"));
    queue.FinishTask("doc-a");
    EXPECT_FALSE(queue.IsRunning("doc-a"));
    queue.FinishTask("doc-a");  // one too many is harmless
    EXPECT_FALSE(queue.IsRunning("doc-a"));
}

TEST(SPCTaskQueueTest, RunningCountsEachTaskOfADocument) {
    SPCTaskQueue queue(10);
    for (int64_t at : {100, 200}) {
        auto t = MakeTask(SPCPriority::kP1, at, "a.txt");
        t->doc_id = "doc-a";
        ASSERT_EQ(queue.Push(t), 0);
    }
    ASSERT_NE(queue.Pop(), nullptr);
    ASSERT_NE(queue.Pop(), nullptr);
    queue.FinishTask("doc-a");
    EXPECT_TRUE(queue.IsRunning("doc-a")) << "the second task still runs";
    queue.FinishTask("doc-a");
    EXPECT_FALSE(queue.IsRunning("doc-a"));

    queue.BeginRunning("doc-b");  // work that does not come through the queue
    EXPECT_TRUE(queue.IsRunning("doc-b"));
    EXPECT_FALSE(queue.IsRunning("doc-a"));
    queue.FinishTask("doc-b");
    EXPECT_FALSE(queue.IsRunning("doc-b"));
}

TEST(SPCTaskQueueTest, ACancelledTaskIsNeitherQueuedNorRunning) {
    SPCTaskQueue queue(10);
    auto a = MakeTask(SPCPriority::kP1, 100, "a.txt");
    a->doc_id = "doc-a";
    ASSERT_EQ(queue.Push(a), 0);
    ASSERT_EQ(queue.CancelQueuedVersions("", "", "a.txt"), 1);
    EXPECT_FALSE(queue.HasQueuedTask("doc-a"));
    EXPECT_FALSE(queue.IsRunning("doc-a"));
    queue.Stop();
    EXPECT_EQ(queue.Pop(), nullptr);  // the cancelled task is skipped, not run
    EXPECT_FALSE(queue.IsRunning("doc-a"));
}

}  // namespace
}  // namespace cortrix
