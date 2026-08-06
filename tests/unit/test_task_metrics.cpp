#include <regex>
#include <gtest/gtest.h>

#include <string>

#include "cortrix/async/task_metrics.h"

// MET-10 coverage: the 6 cortrix_tasks_* metrics — counters/gauge/
// histogram recording + the OpenMetrics renderer + label-enum discipline
// (OBS_SPEC no high-cardinality labels). Note metric names are
// `cortrix_tasks_*` (the locked OBS_SPEC names).
namespace cortrix::async {
namespace {

using CompletionStatus = TaskMetrics::CompletionStatus;
using QueueState = TaskMetrics::QueueState;
using CancelPhase = TaskMetrics::CancelPhase;

class TaskMetricsTest : public ::testing::Test {
protected:
    void SetUp() override { TaskMetrics::Instance().ResetForTest(); }
    void TearDown() override { TaskMetrics::Instance().ResetForTest(); }
    TaskMetrics& M() { return TaskMetrics::Instance(); }
};

TEST_F(TaskMetricsTest, SubmittedCountsByTaskType) {
    M().RecordSubmitted(kTaskDocParse);
    M().RecordSubmitted(kTaskDocParse);
    M().RecordSubmitted(kTaskWatcherFanout);
    EXPECT_EQ(M().SubmittedCount(kTaskDocParse), 2u);
    EXPECT_EQ(M().SubmittedCount(kTaskWatcherFanout), 1u);
    EXPECT_EQ(M().SubmittedCount(kTaskDocSummary), 0u);
}

TEST_F(TaskMetricsTest, CompletedCountsByTaskTypeAndStatus) {
    M().RecordCompleted(kTaskDocParse, CompletionStatus::kSuccess);
    M().RecordCompleted(kTaskDocParse, CompletionStatus::kFailed);
    M().RecordCompleted(kTaskDocParse, CompletionStatus::kSuccess);
    M().RecordCompleted(kTaskDocSummary, CompletionStatus::kTimeout);
    EXPECT_EQ(M().CompletedCount(kTaskDocParse, CompletionStatus::kSuccess), 2u);
    EXPECT_EQ(M().CompletedCount(kTaskDocParse, CompletionStatus::kFailed), 1u);
    EXPECT_EQ(M().CompletedCount(kTaskDocSummary, CompletionStatus::kTimeout), 1u);
    EXPECT_EQ(M().CompletedCount(kTaskDocSummary, CompletionStatus::kCancelled), 0u);
}

TEST_F(TaskMetricsTest, QueueDepthGaugeByStateClamped) {
    M().SetQueueDepth(QueueState::kQueued, 42);
    M().SetQueueDepth(QueueState::kProcessing, 4);
    EXPECT_EQ(M().QueueDepth(QueueState::kQueued), 42);
    EXPECT_EQ(M().QueueDepth(QueueState::kProcessing), 4);
    M().SetQueueDepth(QueueState::kQueued, -7);  // clamped to 0
    EXPECT_EQ(M().QueueDepth(QueueState::kQueued), 0);
}

TEST_F(TaskMetricsTest, ZombieCleanedAndCancelCounters) {
    M().AddZombieCleaned(3);
    M().AddZombieCleaned(2);
    EXPECT_EQ(M().ZombieCleanedCount(), 5u);

    M().RecordCancel(CancelPhase::kPreDequeue);
    M().RecordCancel(CancelPhase::kMidProcessing);
    M().RecordCancel(CancelPhase::kMidProcessing);
    EXPECT_EQ(M().CancelCount(CancelPhase::kPreDequeue), 1u);
    EXPECT_EQ(M().CancelCount(CancelPhase::kMidProcessing), 2u);
    EXPECT_EQ(M().CancelCount(CancelPhase::kPostChunkIdx), 0u);
}

TEST_F(TaskMetricsTest, RenderOpenMetricsHasAllSixMetricsAndTypes) {
    M().RecordSubmitted(kTaskDocParse);
    M().RecordCompleted(kTaskDocParse, CompletionStatus::kSuccess);
    M().ObserveDuration(kTaskDocParse, 12.0);
    M().SetQueueDepth(QueueState::kQueued, 5);
    M().AddZombieCleaned(1);
    M().RecordCancel(CancelPhase::kPreDequeue);

    std::string out = M().RenderOpenMetrics();
    // All 6 metric names present, all under the cortrix_tasks_ prefix.
    EXPECT_NE(out.find("cortrix_tasks_submitted_total"), std::string::npos);
    EXPECT_NE(out.find("cortrix_tasks_completed_total"), std::string::npos);
    EXPECT_NE(out.find("cortrix_tasks_duration_seconds"), std::string::npos);
    EXPECT_NE(out.find("cortrix_tasks_queue_depth"), std::string::npos);
    EXPECT_NE(out.find("cortrix_tasks_zombie_cleaned_total"), std::string::npos);
    EXPECT_NE(out.find("cortrix_tasks_cancel_total"), std::string::npos);
    // No metric name may embed a tracking-style label.
    EXPECT_FALSE(std::regex_search(out, std::regex(R"(cortrix_(f|mem|p)[0-9]{2})")));
    // TYPE lines (counter / gauge / histogram).
    EXPECT_NE(out.find("# TYPE cortrix_tasks_submitted_total counter"), std::string::npos);
    EXPECT_NE(out.find("# TYPE cortrix_tasks_queue_depth gauge"), std::string::npos);
    EXPECT_NE(out.find("# TYPE cortrix_tasks_duration_seconds histogram"), std::string::npos);
}

// The duration histogram renders well-formed cumulative buckets per task_type:
// every declared le bound + +Inf, monotonically non-decreasing, +Inf == _count.
TEST_F(TaskMetricsTest, DurationHistogramBucketsAreCumulativeAndComplete) {
    // Observations across buckets (s): 0.5→le 1, 12→le 15, 120→le 300, 2000→+Inf only.
    M().ObserveDuration(kTaskDocParse, 0.5);
    M().ObserveDuration(kTaskDocParse, 12.0);
    M().ObserveDuration(kTaskDocParse, 120.0);
    M().ObserveDuration(kTaskDocParse, 2000.0);
    std::string out = M().RenderOpenMetrics();

    const std::string prefix =
        "cortrix_tasks_duration_seconds_bucket{task_type=\"kTaskDocParse\",le=\"";
    auto bucket_value = [&](const std::string& le) -> long {
        std::string key = prefix + le + "\"} ";
        size_t p = out.find(key);
        if (p == std::string::npos) return -1;
        p += key.size();
        return std::stol(out.substr(p, out.find('\n', p) - p));
    };

    long prev = 0;
    for (const char* le : {"1", "5", "15", "30", "60", "300", "900", "1800"}) {
        long v = bucket_value(le);
        ASSERT_GE(v, 0) << "missing bucket le=" << le;
        EXPECT_GE(v, prev) << "bucket le=" << le << " not cumulative";
        prev = v;
    }
    EXPECT_EQ(bucket_value("1"), 1);      // 0.5s
    EXPECT_EQ(bucket_value("15"), 2);     // + 12s
    EXPECT_EQ(bucket_value("300"), 3);    // + 120s
    EXPECT_EQ(bucket_value("1800"), 3);   // 2000s only in +Inf
    EXPECT_EQ(bucket_value("+Inf"), 4);   // +Inf == total
    EXPECT_NE(out.find("cortrix_tasks_duration_seconds_count{task_type=\"kTaskDocParse\"} 4"),
              std::string::npos);
}

TEST_F(TaskMetricsTest, RenderHasNoHighCardinalityLabels) {
    // OBS_SPEC: no task_id / namespace_id / doc_id labels ever appear.
    M().RecordSubmitted(kTaskDocParse);
    M().RecordCompleted(kTaskDocParse, CompletionStatus::kSuccess);
    std::string out = M().RenderOpenMetrics();
    EXPECT_EQ(out.find("task_id"), std::string::npos);
    EXPECT_EQ(out.find("namespace_id"), std::string::npos);
    EXPECT_EQ(out.find("doc_id"), std::string::npos);
}

TEST_F(TaskMetricsTest, TaskTypeIndexFoldsUnknownIntoDocParse) {
    // An out-of-range task_type folds into the doc-parse slot (bounded recorder).
    EXPECT_EQ(TaskTypeIndex(kTaskDocParse), 0);
    EXPECT_EQ(TaskTypeIndex(kTaskWatcherFanout), 1);
    EXPECT_EQ(TaskTypeIndex(kTaskDocSummary), 2);
    EXPECT_EQ(TaskTypeIndex(static_cast<TaskType>(99)), 0);
}

TEST_F(TaskMetricsTest, LabelStringsMatchSpec) {
    EXPECT_STREQ(ToString(kTaskDocParse), "kTaskDocParse");
    EXPECT_STREQ(ToString(kTaskWatcherFanout), "kTaskWatcherFanout");
    EXPECT_STREQ(ToString(kTaskDocSummary), "kTaskDocSummary");
    EXPECT_STREQ(ToString(CompletionStatus::kSuccess), "success");
    EXPECT_STREQ(ToString(CompletionStatus::kFailed), "failed");
    EXPECT_STREQ(ToString(CompletionStatus::kCancelled), "cancelled");
    EXPECT_STREQ(ToString(CompletionStatus::kTimeout), "timeout");
    EXPECT_STREQ(ToString(QueueState::kQueued), "queued");
    EXPECT_STREQ(ToString(QueueState::kProcessing), "processing");
    EXPECT_STREQ(ToString(CancelPhase::kPreDequeue), "pre_dequeue");
    EXPECT_STREQ(ToString(CancelPhase::kMidProcessing), "mid_processing");
    EXPECT_STREQ(ToString(CancelPhase::kPostChunkIdx), "post_chunk_idx");
}

}  // namespace
}  // namespace cortrix::async
