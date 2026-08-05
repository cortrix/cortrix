#include <gtest/gtest.h>

#include <set>
#include <string>
#include <vector>

#include "cortrix/async/task_metrics.h"
#include "cortrix/async/task_type.h"

// Exhaustive ToString matrices for the async-task enums (cortrix::async,
// src/async/task_metrics.cpp). Both are one-way (ToString only):
//   TaskType (unscoped enum, stable values 1/2/3) -> "kTask*" debug labels.
//   TaskMetrics::CompletionStatus (4 values) -> success/failed/cancelled/timeout.
// Suite names unique with RtAsync* prefix.
namespace cortrix::async {
namespace {

// ----------------------------------------------------------------------------
// TaskType: 3 stable values. Tokens are the enum identifier strings.
// ----------------------------------------------------------------------------
const std::vector<TaskType>& AllTaskTypes() {
    static const std::vector<TaskType> v = {
        kTaskDocParse, kTaskWatcherFanout, kTaskDocSummary};
    return v;
}

class RtAsyncTaskTypeMatrix : public ::testing::TestWithParam<TaskType> {};

TEST_P(RtAsyncTaskTypeMatrix, TokenNonEmpty) {
    const std::string token = ToString(GetParam());
    EXPECT_FALSE(token.empty());
}

INSTANTIATE_TEST_SUITE_P(AllValues, RtAsyncTaskTypeMatrix,
                         ::testing::ValuesIn(AllTaskTypes()));

TEST(RtAsyncTaskTypeTokens, ExactStringsValuesAndUniqueness) {
    // Stable underlying values (async task backward-compat contract).
    EXPECT_EQ(static_cast<int>(kTaskDocParse), 1);
    EXPECT_EQ(static_cast<int>(kTaskWatcherFanout), 2);
    EXPECT_EQ(static_cast<int>(kTaskDocSummary), 3);

    EXPECT_STREQ(ToString(kTaskDocParse), "kTaskDocParse");
    EXPECT_STREQ(ToString(kTaskWatcherFanout), "kTaskWatcherFanout");
    EXPECT_STREQ(ToString(kTaskDocSummary), "kTaskDocSummary");

    std::set<std::string> seen;
    for (TaskType t : AllTaskTypes())
        EXPECT_TRUE(seen.insert(ToString(t)).second);
    EXPECT_EQ(seen.size(), 3u);
}

// ----------------------------------------------------------------------------
// TaskMetrics::CompletionStatus: 4 values.
// ----------------------------------------------------------------------------
using CompletionStatus = TaskMetrics::CompletionStatus;

const std::vector<CompletionStatus>& AllCompletionStatuses() {
    static const std::vector<CompletionStatus> v = {
        CompletionStatus::kSuccess, CompletionStatus::kFailed,
        CompletionStatus::kCancelled, CompletionStatus::kTimeout};
    return v;
}

class RtAsyncCompletionStatusMatrix
    : public ::testing::TestWithParam<CompletionStatus> {};

TEST_P(RtAsyncCompletionStatusMatrix, TokenNonEmptyLowercase) {
    const std::string token = ToString(GetParam());
    EXPECT_FALSE(token.empty());
    for (char c : token) {
        EXPECT_TRUE(c >= 'a' && c <= 'z');
    }
}

INSTANTIATE_TEST_SUITE_P(AllValues, RtAsyncCompletionStatusMatrix,
                         ::testing::ValuesIn(AllCompletionStatuses()));

TEST(RtAsyncCompletionStatusTokens, ExactStringsAndUniqueness) {
    EXPECT_STREQ(ToString(CompletionStatus::kSuccess), "success");
    EXPECT_STREQ(ToString(CompletionStatus::kFailed), "failed");
    EXPECT_STREQ(ToString(CompletionStatus::kCancelled), "cancelled");
    EXPECT_STREQ(ToString(CompletionStatus::kTimeout), "timeout");
    std::set<std::string> seen;
    for (CompletionStatus s : AllCompletionStatuses())
        EXPECT_TRUE(seen.insert(ToString(s)).second);
    EXPECT_EQ(seen.size(), 4u);
}

}  // namespace
}  // namespace cortrix::async
