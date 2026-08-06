#include <gtest/gtest.h>

#include <string>

#include "cortrix/memory/memory_isolation_metrics.h"

// MET-09 coverage: the 7 cortrix_memory_isolation_* metrics — counters/gauges
// recording + the OpenMetrics renderer + label-enum discipline (OBS_SPEC §3.2 no
// high-cardinality labels, esp. no user_id).
namespace cortrix::memory {
namespace {

using CheckResult = MemoryIsolationMetrics::CheckResult;
using Action = MemoryIsolationMetrics::Action;
using Reason = MemoryIsolationMetrics::Reason;
using QuotaType = MemoryIsolationMetrics::QuotaType;

class MemoryIsolationMetricsTest : public ::testing::Test {
protected:
    void SetUp() override { MemoryIsolationMetrics::Instance().ResetForTest(); }
    void TearDown() override { MemoryIsolationMetrics::Instance().ResetForTest(); }
    MemoryIsolationMetrics& M() { return MemoryIsolationMetrics::Instance(); }
};

TEST_F(MemoryIsolationMetricsTest, IsolationCheckCountsByResultAndAction) {
    M().RecordIsolationCheck(CheckResult::kPass, Action::kSearch);
    M().RecordIsolationCheck(CheckResult::kPass, Action::kSearch);
    M().RecordIsolationCheck(CheckResult::kViolation, Action::kDelete);
    EXPECT_EQ(M().IsolationCheckCount(CheckResult::kPass, Action::kSearch), 2u);
    EXPECT_EQ(M().IsolationCheckCount(CheckResult::kViolation, Action::kDelete), 1u);
    EXPECT_EQ(M().IsolationCheckCount(CheckResult::kPass, Action::kDelete), 0u);
}

TEST_F(MemoryIsolationMetricsTest, IsolationViolationIsTheSafetyAlertMetric) {
    M().RecordIsolationViolation(Action::kEdit, Reason::kMismatch);
    M().RecordIsolationViolation(Action::kEdit, Reason::kMismatch);
    M().RecordIsolationViolation(Action::kSession, Reason::kMissingUserId);
    EXPECT_EQ(M().IsolationViolationCount(Action::kEdit, Reason::kMismatch), 2u);
    EXPECT_EQ(M().IsolationViolationCount(Action::kSession, Reason::kMissingUserId), 1u);
    EXPECT_EQ(M().IsolationViolationCount(Action::kList, Reason::kMismatch), 0u);
}

TEST_F(MemoryIsolationMetricsTest, QuotaExceededCountsByType) {
    M().RecordQuotaExceeded(QuotaType::kItemsCount);
    M().RecordQuotaExceeded(QuotaType::kMemoryBytes);
    M().RecordQuotaExceeded(QuotaType::kMemoryBytes);
    EXPECT_EQ(M().QuotaExceededCount(QuotaType::kItemsCount), 1u);
    EXPECT_EQ(M().QuotaExceededCount(QuotaType::kMemoryBytes), 2u);
    EXPECT_EQ(M().QuotaExceededCount(QuotaType::kSessionCount), 0u);
}

TEST_F(MemoryIsolationMetricsTest, QuotaUsageRatioGaugeSetAndClamped) {
    M().SetQuotaUsageRatio(QuotaType::kItemsCount, 0.42);
    EXPECT_DOUBLE_EQ(M().QuotaUsageRatio(QuotaType::kItemsCount), 0.42);
    M().SetQuotaUsageRatio(QuotaType::kItemsCount, 1.5);   // clamped to 1.0
    EXPECT_DOUBLE_EQ(M().QuotaUsageRatio(QuotaType::kItemsCount), 1.0);
    M().SetQuotaUsageRatio(QuotaType::kSessionCount, -0.2); // clamped to 0.0
    EXPECT_DOUBLE_EQ(M().QuotaUsageRatio(QuotaType::kSessionCount), 0.0);
}

TEST_F(MemoryIsolationMetricsTest, UserSessionCountGaugeSetAndClamped) {
    M().SetUserSessionCount(13);
    EXPECT_EQ(M().UserSessionCount(), 13);
    M().SetUserSessionCount(-4);  // clamped to 0
    EXPECT_EQ(M().UserSessionCount(), 0);
}

TEST_F(MemoryIsolationMetricsTest, DefaultUserUsedAndMatchScopeExcluded) {
    M().RecordDefaultUserUsed();
    M().RecordDefaultUserUsed();
    EXPECT_EQ(M().DefaultUserUsedCount(), 2u);

    M().RecordMatchScopeExcluded(Reason::kMissingUserId);
    M().RecordMatchScopeExcluded(Reason::kMismatch);
    M().RecordMatchScopeExcluded(Reason::kMismatch);
    EXPECT_EQ(M().MatchScopeExcludedCount(Reason::kMissingUserId), 1u);
    EXPECT_EQ(M().MatchScopeExcludedCount(Reason::kMismatch), 2u);
    EXPECT_EQ(M().MatchScopeExcludedCount(Reason::kEmptyUserId), 0u);
}

TEST_F(MemoryIsolationMetricsTest, RenderOpenMetricsHasAllSevenMetricsAndTypes) {
    M().RecordIsolationCheck(CheckResult::kPass, Action::kSearch);
    M().RecordIsolationViolation(Action::kDelete, Reason::kMismatch);
    M().RecordQuotaExceeded(QuotaType::kItemsCount);
    M().SetQuotaUsageRatio(QuotaType::kItemsCount, 0.9);
    M().SetUserSessionCount(5);
    M().RecordDefaultUserUsed();
    M().RecordMatchScopeExcluded(Reason::kMismatch);

    std::string out = M().RenderOpenMetrics();
    // All 7 metric names present.
    EXPECT_NE(out.find("cortrix_memory_isolation_check_total"), std::string::npos);
    EXPECT_NE(out.find("cortrix_memory_isolation_violation_total"), std::string::npos);
    EXPECT_NE(out.find("cortrix_memory_isolation_quota_exceeded_total"), std::string::npos);
    EXPECT_NE(out.find("cortrix_memory_isolation_quota_usage_ratio"), std::string::npos);
    EXPECT_NE(out.find("cortrix_memory_isolation_user_session_count"), std::string::npos);
    EXPECT_NE(out.find("cortrix_memory_isolation_default_user_used_total"), std::string::npos);
    EXPECT_NE(out.find("cortrix_memory_isolation_match_scope_excluded_total"), std::string::npos);
    // TYPE lines (counter / gauge mix).
    EXPECT_NE(out.find("# TYPE cortrix_memory_isolation_check_total counter"), std::string::npos);
    EXPECT_NE(out.find("# TYPE cortrix_memory_isolation_violation_total counter"), std::string::npos);
    EXPECT_NE(out.find("# TYPE cortrix_memory_isolation_quota_usage_ratio gauge"), std::string::npos);
    EXPECT_NE(out.find("# TYPE cortrix_memory_isolation_user_session_count gauge"), std::string::npos);
    // Safety alert metric exposes its labels.
    EXPECT_NE(out.find("cortrix_memory_isolation_violation_total{action=\"delete\",reason=\"mismatch\"}"),
              std::string::npos);
}

TEST_F(MemoryIsolationMetricsTest, RenderHasNoHighCardinalityLabels) {
    // Memory isolation / OBS_SPEC §3.2: user_id is on the absolute deny list. Labels
    // are enum-only; per-user data goes through the audit log, never a label.
    M().RecordIsolationCheck(CheckResult::kPass, Action::kSearch);
    M().RecordDefaultUserUsed();
    std::string out = M().RenderOpenMetrics();
    // Check each forbidden field as a LABEL KEY (`<key>="..."`), not a bare
    // substring: the match_scope_excluded reason enum legitimately contains
    // low-cardinality values like "missing_user_id" / "empty_user_id" — reason is
    // a bounded enum label, not the OBS_SPEC §3.2-forbidden high-cardinality user_id key.
    EXPECT_EQ(out.find("user_id=\""), std::string::npos);
    EXPECT_EQ(out.find("tenant_id=\""), std::string::npos);
    EXPECT_EQ(out.find("ns_id=\""), std::string::npos);
    EXPECT_EQ(out.find("session_id=\""), std::string::npos);
}

TEST_F(MemoryIsolationMetricsTest, LabelStringsMatchSpec) {
    EXPECT_STREQ(ToString(CheckResult::kPass), "pass");
    EXPECT_STREQ(ToString(CheckResult::kViolation), "violation");
    EXPECT_STREQ(ToString(Action::kSearch), "search");
    EXPECT_STREQ(ToString(Action::kList), "list");
    EXPECT_STREQ(ToString(Action::kEdit), "edit");
    EXPECT_STREQ(ToString(Action::kDelete), "delete");
    EXPECT_STREQ(ToString(Action::kSession), "session");
    EXPECT_STREQ(ToString(Action::kSessionAccess), "session_access");
    EXPECT_STREQ(ToString(Reason::kMissingUserId), "missing_user_id");
    EXPECT_STREQ(ToString(Reason::kEmptyUserId), "empty_user_id");
    EXPECT_STREQ(ToString(Reason::kMismatch), "mismatch");
    EXPECT_STREQ(ToString(QuotaType::kItemsCount), "items_count");
    EXPECT_STREQ(ToString(QuotaType::kSessionCount), "session_count");
    EXPECT_STREQ(ToString(QuotaType::kMemoryBytes), "memory_bytes");
}

}  // namespace
}  // namespace cortrix::memory
