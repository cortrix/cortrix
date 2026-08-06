// Async task WorkerPool task_type dispatch (T4): the pool routes each dequeued TaskInfo to
// the ITaskHandler registered for its task_type (kTaskDocParse → DocumentProcessor,
// kTaskDocSummary → DocSummaryAsyncWorker in production). Here the handlers are recording
// doubles so the test asserts the routing itself over the real TaskScheduler /
// TaskManager / worker threads, without the parser or doc summary generator.
#include "cortrix/async/worker_pool.h"

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "cortrix/async/task_handler.h"
#include "cortrix/async/task_info.h"
#include "cortrix/async/task_manager.h"
#include "cortrix/async/task_scheduler.h"
#include "cortrix/async/task_type.h"
#include "cortrix/common/in_memory_global_config.h"

namespace cortrix::async {
namespace {

// Records the task_type of every task it is handed; lets a test block until a count.
class RecordingHandler : public ITaskHandler {
public:
    Status ProcessTask(const TaskInfo& task) override {
        {
            std::lock_guard<std::mutex> lk(mu_);
            seen_.push_back(task.task_type);
        }
        cv_.notify_all();
        return Status::Ok();
    }
    bool WaitFor(size_t count, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lk(mu_);
        return cv_.wait_for(lk, timeout, [&] { return seen_.size() >= count; });
    }
    std::vector<int> seen() {
        std::lock_guard<std::mutex> lk(mu_);
        return seen_;
    }

private:
    std::mutex mu_;
    std::condition_variable cv_;
    std::vector<int> seen_;
};

class WorkerPoolDispatchTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(mgr_.Init(":memory:").ok());
        cfg_.Set("async.worker_pool_size", "2");
        cfg_.Set("parser.parser_max_concurrent", "4");
        sched_ = std::make_unique<TaskScheduler>(&mgr_, &cfg_);
    }

    void Enqueue(int task_type, const std::string& doc_id) {
        SubmitRequest req;
        req.namespace_id = "ns";
        req.filename = "f.bin";
        req.filepath = "/tmp/" + doc_id;
        req.doc_id = doc_id;
        req.content_hash = doc_id;  // distinct per doc → no debounce collisions
        req.total_pages = 1;
        req.task_type = task_type;
        ASSERT_TRUE(sched_->Enqueue(req).ok());
    }

    TaskManager mgr_;
    InMemoryGlobalConfig cfg_;
    std::unique_ptr<TaskScheduler> sched_;
};

TEST_F(WorkerPoolDispatchTest, RoutesEachTaskTypeToItsHandler) {
    RecordingHandler parse_h, summary_h;
    WorkerPool pool(sched_.get(), &cfg_);
    pool.RegisterHandler(kTaskDocParse, &parse_h);
    pool.RegisterHandler(kTaskDocSummary, &summary_h);
    ASSERT_TRUE(pool.Start().ok());

    Enqueue(kTaskDocSummary, "doc-sum-1");
    pool.Notify();
    ASSERT_TRUE(summary_h.WaitFor(1, std::chrono::seconds(2)));

    Enqueue(kTaskDocParse, "doc-parse-1");
    pool.Notify();
    ASSERT_TRUE(parse_h.WaitFor(1, std::chrono::seconds(2)));

    pool.Stop();
    EXPECT_EQ(summary_h.seen(), (std::vector<int>{kTaskDocSummary}));
    EXPECT_EQ(parse_h.seen(), (std::vector<int>{kTaskDocParse}));
}

TEST_F(WorkerPoolDispatchTest, UnregisteredTaskTypeIsLoggedNotCrashed) {
    RecordingHandler parse_h;
    WorkerPool pool(sched_.get(), &cfg_);
    pool.RegisterHandler(kTaskDocParse, &parse_h);  // no handler for kTaskDocSummary
    ASSERT_TRUE(pool.Start().ok());

    // An unhandled task_type must be skipped (logged) without crashing the worker.
    Enqueue(kTaskDocSummary, "doc-unhandled");
    pool.Notify();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // A registered type still dispatches after an unhandled one.
    Enqueue(kTaskDocParse, "doc-parse-2");
    pool.Notify();
    ASSERT_TRUE(parse_h.WaitFor(1, std::chrono::seconds(2)));

    pool.Stop();
    EXPECT_EQ(parse_h.seen(), (std::vector<int>{kTaskDocParse}));
}

}  // namespace
}  // namespace cortrix::async
