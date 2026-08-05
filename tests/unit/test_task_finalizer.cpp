// TaskFinalizer (async task, finalize ownership = handler · decision A): the shared
// terminal-write + async task task-metric collapse used by DocumentProcessor / F41AsyncWorker.
// These verify each terminal path drives the tasks table to the right status AND bumps
// cortrix_tasks_completed_total{status}, over a real in-memory TaskManager.
#include "cortrix/async/task_finalizer.h"

#include <gtest/gtest.h>

#include <chrono>
#include <string>

#include <nlohmann/json.hpp>

#include "cortrix/async/f42_metrics.h"
#include "cortrix/async/task_info.h"
#include "cortrix/async/task_manager.h"
#include "cortrix/async/task_type.h"

namespace cortrix::async {
namespace {

using Comp = F42Metrics::CompletionStatus;

class TaskFinalizerTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(mgr_.Init(":memory:").ok());
        F42Metrics::Instance().ResetForTest();
    }

    // Create a task and drive it to `processing` (the state a Worker finalizes from).
    TaskInfo MakeProcessingTask(int task_type) {
        TaskInfo t;
        t.namespace_id = "ns";
        t.doc_id = "doc-" + std::to_string(++seq_);
        t.content_hash = t.doc_id;
        t.task_type = task_type;
        t.status = task_status::kQueued;
        auto created = mgr_.CreateTask(t);
        EXPECT_TRUE(created.ok());
        EXPECT_TRUE(mgr_.MarkProcessing(created.value().task_id, /*worker_id=*/1).ok());
        return mgr_.GetTask(created.value().task_id).value();
    }

    TaskManager mgr_;
    TaskFinalizer finalizer_{&mgr_};
    int seq_ = 0;
};

TEST_F(TaskFinalizerTest, CompleteMarksCompletedAndRecordsSuccess) {
    TaskInfo t = MakeProcessingTask(kTaskDocSummary);
    Status s = finalizer_.Complete(t, "doc-final", std::chrono::steady_clock::now());
    EXPECT_TRUE(s.ok()) << s.message();

    auto got = mgr_.GetTask(t.task_id);
    ASSERT_TRUE(got.ok());
    EXPECT_EQ(got.value().status, task_status::kCompleted);
    EXPECT_EQ(got.value().doc_id, "doc-final");
    EXPECT_EQ(F42Metrics::Instance().CompletedCount(TaskType::kTaskDocSummary, Comp::kSuccess), 1u);
}

TEST_F(TaskFinalizerTest, FailMarksFailedWithDomainCodeAndRecordsFailed) {
    TaskInfo t = MakeProcessingTask(kTaskDocSummary);
    nlohmann::json sd = {{"doc_id", t.doc_id}};
    // A handler-domain code NOT in F42ErrorCode — finalizer must still carry it through.
    Status s = finalizer_.Fail(t, "CX_ERR_F41_GENERATION_FAILED", "llm timeout", sd,
                               std::chrono::steady_clock::now());
    EXPECT_FALSE(s.ok());
    EXPECT_NE(s.message().find("CX_ERR_F41_GENERATION_FAILED"), std::string::npos);

    auto got = mgr_.GetTask(t.task_id);
    ASSERT_TRUE(got.ok());
    EXPECT_EQ(got.value().status, task_status::kFailed);
    EXPECT_EQ(got.value().error_code, "CX_ERR_F41_GENERATION_FAILED");
    EXPECT_EQ(F42Metrics::Instance().CompletedCount(TaskType::kTaskDocSummary, Comp::kFailed), 1u);
}

TEST_F(TaskFinalizerTest, CancelMarksCancelledAndRecordsCancelled) {
    TaskInfo t = MakeProcessingTask(kTaskDocParse);
    Status s = finalizer_.Cancel(t, std::chrono::steady_clock::now());
    EXPECT_FALSE(s.ok());
    EXPECT_NE(s.message().find("CX_ERR_TASK_CANCELLING"), std::string::npos);

    auto got = mgr_.GetTask(t.task_id);
    ASSERT_TRUE(got.ok());
    EXPECT_EQ(got.value().status, task_status::kCancelled);
    EXPECT_EQ(F42Metrics::Instance().CompletedCount(TaskType::kTaskDocParse, Comp::kCancelled), 1u);
}

// task_type scopes the metric: a doc-parse completion must not bump the doc-summary slot.
TEST_F(TaskFinalizerTest, MetricScopedByTaskType) {
    TaskInfo a = MakeProcessingTask(kTaskDocParse);
    TaskInfo b = MakeProcessingTask(kTaskDocSummary);
    finalizer_.Complete(a, "da", std::chrono::steady_clock::now());
    finalizer_.Complete(b, "db", std::chrono::steady_clock::now());
    EXPECT_EQ(F42Metrics::Instance().CompletedCount(TaskType::kTaskDocParse, Comp::kSuccess), 1u);
    EXPECT_EQ(F42Metrics::Instance().CompletedCount(TaskType::kTaskDocSummary, Comp::kSuccess), 1u);
}

// Finalize persist failure is non-fatal: a vanished task row (concurrent delete) still
// reports business success — mirrors the DocumentProcessor "parse OK stays OK" contract.
TEST_F(TaskFinalizerTest, CompleteToleratesMissingTaskRowReturnsOk) {
    TaskInfo phantom;
    phantom.task_id = "phantom-task";  // never inserted → MarkCompleted NotFounds
    phantom.task_type = kTaskDocSummary;
    Status s = finalizer_.Complete(phantom, "doc", std::chrono::steady_clock::now());
    EXPECT_TRUE(s.ok()) << s.message();
    EXPECT_EQ(F42Metrics::Instance().CompletedCount(TaskType::kTaskDocSummary, Comp::kSuccess), 1u);
}

}  // namespace
}  // namespace cortrix::async
