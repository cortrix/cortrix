// Async task TaskMetrics — exhaustive matrix coverage (TaskType + CompletionStatus +
// QueueState + CancelPhase ToString, (task_type x status) combos, duration
// histogram boundaries, queue_depth gauge, RenderOpenMetrics, ResetForTest).
// Separate suite namespace from test_task_metrics.cpp.
#include <regex>
#include <gtest/gtest.h>

#include <array>
#include <string>
#include <tuple>

#include "cortrix/async/task_metrics.h"
#include "cortrix/async/task_type.h"

namespace cortrix::async {
namespace matrix_test {

using CS = TaskMetrics::CompletionStatus;
using QS = TaskMetrics::QueueState;
using CP = TaskMetrics::CancelPhase;

constexpr std::array<TaskType, 3> kTypes{kTaskDocParse, kTaskWatcherFanout,
                                         kTaskDocSummary};
constexpr std::array<CS, 4> kStatuses{CS::kSuccess, CS::kFailed, CS::kCancelled,
                                      CS::kTimeout};
constexpr std::array<QS, 2> kStates{QS::kQueued, QS::kProcessing};
constexpr std::array<CP, 3> kPhases{CP::kPreDequeue, CP::kMidProcessing,
                                    CP::kPostChunkIdx};

class TaskAsyncFx : public ::testing::Test {
protected:
    void SetUp() override { TaskMetrics::Instance().ResetForTest(); }
    void TearDown() override { TaskMetrics::Instance().ResetForTest(); }
    TaskMetrics& m() { return TaskMetrics::Instance(); }
};

TEST_F(TaskAsyncFx, ToStringTaskType) {
    EXPECT_STREQ(ToString(kTaskDocParse), "kTaskDocParse");
    EXPECT_STREQ(ToString(kTaskWatcherFanout), "kTaskWatcherFanout");
    EXPECT_STREQ(ToString(kTaskDocSummary), "kTaskDocSummary");
}
TEST_F(TaskAsyncFx, ToStringCompletionStatus) {
    EXPECT_STREQ(ToString(CS::kSuccess), "success");
    EXPECT_STREQ(ToString(CS::kFailed), "failed");
    EXPECT_STREQ(ToString(CS::kCancelled), "cancelled");
    EXPECT_STREQ(ToString(CS::kTimeout), "timeout");
}
TEST_F(TaskAsyncFx, ToStringQueueState) {
    EXPECT_STREQ(ToString(QS::kQueued), "queued");
    EXPECT_STREQ(ToString(QS::kProcessing), "processing");
}
TEST_F(TaskAsyncFx, ToStringCancelPhase) {
    EXPECT_STREQ(ToString(CP::kPreDequeue), "pre_dequeue");
    EXPECT_STREQ(ToString(CP::kMidProcessing), "mid_processing");
    EXPECT_STREQ(ToString(CP::kPostChunkIdx), "post_chunk_idx");
}
TEST_F(TaskAsyncFx, TaskTypeIndexMapping) {
    EXPECT_EQ(TaskTypeIndex(kTaskDocParse), 0);
    EXPECT_EQ(TaskTypeIndex(kTaskWatcherFanout), 1);
    EXPECT_EQ(TaskTypeIndex(kTaskDocSummary), 2);
}

// submitted_total per task_type
class TaskAsyncSubmittedMatrix : public TaskAsyncFx,
                        public ::testing::WithParamInterface<TaskType> {};
TEST_P(TaskAsyncSubmittedMatrix, Increments) {
    m().RecordSubmitted(GetParam());
    m().RecordSubmitted(GetParam());
    EXPECT_EQ(m().SubmittedCount(GetParam()), 2u);
    for (TaskType t : kTypes)
        if (t != GetParam()) EXPECT_EQ(m().SubmittedCount(t), 0u);
}
INSTANTIATE_TEST_SUITE_P(All, TaskAsyncSubmittedMatrix, ::testing::ValuesIn(kTypes));

// completed_total: every (task_type x status) cell
using CompParam = std::tuple<TaskType, CS>;
class TaskAsyncCompletedMatrix : public TaskAsyncFx,
                        public ::testing::WithParamInterface<CompParam> {};
TEST_P(TaskAsyncCompletedMatrix, IncrementsOnlyTargetCell) {
    auto [t, st] = GetParam();
    m().RecordCompleted(t, st);
    EXPECT_EQ(m().CompletedCount(t, st), 1u);
    for (TaskType tt : kTypes)
        for (CS s : kStatuses)
            if (!(tt == t && s == st)) EXPECT_EQ(m().CompletedCount(tt, s), 0u);
}
INSTANTIATE_TEST_SUITE_P(AllCells, TaskAsyncCompletedMatrix,
                         ::testing::Combine(::testing::ValuesIn(kTypes),
                                            ::testing::ValuesIn(kStatuses)));

// cancel_total per phase
class TaskAsyncCancelMatrix : public TaskAsyncFx, public ::testing::WithParamInterface<CP> {};
TEST_P(TaskAsyncCancelMatrix, Increments) {
    m().RecordCancel(GetParam());
    EXPECT_EQ(m().CancelCount(GetParam()), 1u);
}
INSTANTIATE_TEST_SUITE_P(All, TaskAsyncCancelMatrix, ::testing::ValuesIn(kPhases));

// queue_depth gauge per state (set/read, last-write-wins)
class TaskAsyncQueueMatrix : public TaskAsyncFx, public ::testing::WithParamInterface<QS> {};
TEST_P(TaskAsyncQueueMatrix, SetReadLastWriteWins) {
    m().SetQueueDepth(GetParam(), 7);
    EXPECT_EQ(m().QueueDepth(GetParam()), 7);
    m().SetQueueDepth(GetParam(), 3);
    EXPECT_EQ(m().QueueDepth(GetParam()), 3);
}
INSTANTIATE_TEST_SUITE_P(All, TaskAsyncQueueMatrix, ::testing::ValuesIn(kStates));

TEST_F(TaskAsyncFx, ZombieCleanedCounter) {
    m().AddZombieCleaned(4);
    m().AddZombieCleaned(1);
    EXPECT_EQ(m().ZombieCleanedCount(), 5u);
}

// duration histogram bounds {1,5,15,30,60,300,900,1800}s.
TEST_F(TaskAsyncFx, DurationHistogramRenderAndSum) {
    m().ObserveDuration(kTaskDocParse, 0.5);   // under first bound
    m().ObserveDuration(kTaskDocParse, 1.0);   // exactly at first bound
    m().ObserveDuration(kTaskDocParse, 9999.0);  // above largest -> only +Inf
    std::string out = m().RenderOpenMetrics();
    EXPECT_NE(out.find("# TYPE cortrix_tasks_duration_seconds histogram"),
              std::string::npos);
    EXPECT_NE(out.find("cortrix_tasks_duration_seconds_count{task_type=\"kTaskDocParse\"} 3"),
              std::string::npos);
    // le="1" cumulative holds the 0.5 and the exactly-1.0 sample.
    EXPECT_NE(out.find("cortrix_tasks_duration_seconds_bucket{task_type=\"kTaskDocParse\",le=\"1\"} 2"),
              std::string::npos);
}

TEST_F(TaskAsyncFx, DurationNegativeClampsToSmallest) {
    m().ObserveDuration(kTaskDocSummary, -10.0);
    std::string out = m().RenderOpenMetrics();
    EXPECT_NE(out.find("cortrix_tasks_duration_seconds_bucket{task_type=\"kTaskDocSummary\",le=\"1\"} 1"),
              std::string::npos);
    EXPECT_NE(out.find("cortrix_tasks_duration_seconds_bucket{task_type=\"kTaskDocSummary\",le=\"+Inf\"} 1"),
              std::string::npos);
}

TEST_F(TaskAsyncFx, RenderHasTypeHelpAndStableSeries) {
    m().RecordSubmitted(kTaskDocParse);
    m().RecordCompleted(kTaskDocParse, CS::kSuccess);
    m().SetQueueDepth(QS::kQueued, 2);
    m().AddZombieCleaned(1);
    m().RecordCancel(CP::kPreDequeue);
    std::string out = m().RenderOpenMetrics();
    EXPECT_NE(out.find("# TYPE cortrix_tasks_submitted_total counter"),
              std::string::npos);
    EXPECT_NE(out.find("# TYPE cortrix_tasks_completed_total counter"),
              std::string::npos);
    EXPECT_NE(out.find("# TYPE cortrix_tasks_queue_depth gauge"),
              std::string::npos);
    EXPECT_NE(out.find("# TYPE cortrix_tasks_zombie_cleaned_total counter"),
              std::string::npos);
    EXPECT_NE(out.find("# TYPE cortrix_tasks_cancel_total counter"),
              std::string::npos);
    // Names are all cortrix_tasks_*; none may embed a tracking-style label.
    EXPECT_FALSE(std::regex_search(out, std::regex(R"(cortrix_(f|mem|p)[0-9]{2})")));
}

TEST_F(TaskAsyncFx, RenderNoHighCardinalityLabels) {
    m().RecordSubmitted(kTaskDocParse);
    std::string out = m().RenderOpenMetrics();
    EXPECT_EQ(out.find("task_id="), std::string::npos);
    EXPECT_EQ(out.find("namespace_id="), std::string::npos);
    EXPECT_EQ(out.find("doc_id="), std::string::npos);
}

TEST_F(TaskAsyncFx, ResetClearsEverything) {
    m().RecordSubmitted(kTaskDocParse);
    m().RecordCompleted(kTaskDocParse, CS::kSuccess);
    m().ObserveDuration(kTaskDocParse, 1.0);
    m().SetQueueDepth(QS::kQueued, 5);
    m().AddZombieCleaned(3);
    m().RecordCancel(CP::kPreDequeue);
    m().ResetForTest();
    EXPECT_EQ(m().SubmittedCount(kTaskDocParse), 0u);
    EXPECT_EQ(m().CompletedCount(kTaskDocParse, CS::kSuccess), 0u);
    EXPECT_EQ(m().QueueDepth(QS::kQueued), 0);
    EXPECT_EQ(m().ZombieCleanedCount(), 0u);
    EXPECT_EQ(m().CancelCount(CP::kPreDequeue), 0u);
}

}  // namespace matrix_test
}  // namespace cortrix::async
