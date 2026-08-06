#include <gtest/gtest.h>

#include <string>

#include "cortrix/memory/memory_opt_out_metrics.h"

// S6 coverage: the memory opt-out `memory_opt_out` subsystem metrics (/ the observability spec) —
// the 3 counters (opt_out_total{triggered_by} / opt_out_revoke_total /
// extract_skipped_total), their increments, and the OpenMetrics text rendering.
namespace cortrix::memory::immunity {
namespace {

using TB = MemoryOptOutMetrics::TriggeredBy;

class MemoryOptOutMetricsTest : public ::testing::Test {
protected:
    void SetUp() override { MemoryOptOutMetrics::Instance().ResetForTest(); }
    void TearDown() override { MemoryOptOutMetrics::Instance().ResetForTest(); }
    MemoryOptOutMetrics& m() { return MemoryOptOutMetrics::Instance(); }
};

TEST_F(MemoryOptOutMetricsTest, StartsAtZero) {
    EXPECT_EQ(m().OptOutCount(TB::kUser), 0u);
    EXPECT_EQ(m().OptOutCount(TB::kAgent), 0u);
    EXPECT_EQ(m().OptOutCount(TB::kSystem), 0u);
    EXPECT_EQ(m().OptOutRevokeCount(), 0u);
    EXPECT_EQ(m().ExtractSkippedCount(), 0u);
}

TEST_F(MemoryOptOutMetricsTest, RecordOptOutPerTrigger) {
    m().RecordOptOut(TB::kUser);
    m().RecordOptOut(TB::kUser);
    m().RecordOptOut(TB::kAgent);
    EXPECT_EQ(m().OptOutCount(TB::kUser), 2u);
    EXPECT_EQ(m().OptOutCount(TB::kAgent), 1u);
    EXPECT_EQ(m().OptOutCount(TB::kSystem), 0u);
}

TEST_F(MemoryOptOutMetricsTest, RecordRevokeAndSkipped) {
    m().RecordOptOutRevoke();
    m().RecordExtractSkipped();
    m().RecordExtractSkipped();
    m().RecordExtractSkipped();
    EXPECT_EQ(m().OptOutRevokeCount(), 1u);
    EXPECT_EQ(m().ExtractSkippedCount(), 3u);
}

TEST_F(MemoryOptOutMetricsTest, ResetForTestClearsAll) {
    m().RecordOptOut(TB::kSystem);
    m().RecordOptOutRevoke();
    m().RecordExtractSkipped();
    m().ResetForTest();
    EXPECT_EQ(m().OptOutCount(TB::kSystem), 0u);
    EXPECT_EQ(m().OptOutRevokeCount(), 0u);
    EXPECT_EQ(m().ExtractSkippedCount(), 0u);
}

TEST_F(MemoryOptOutMetricsTest, RenderOpenMetricsHasAllSeries) {
    m().RecordOptOut(TB::kUser);
    m().RecordOptOut(TB::kAgent);
    m().RecordOptOutRevoke();
    m().RecordExtractSkipped();
    const std::string out = m().RenderOpenMetrics();

    // TYPE/HELP headers for all 3 metrics.
    EXPECT_NE(out.find("# TYPE cortrix_memory_opt_out_total counter"), std::string::npos);
    EXPECT_NE(out.find("# TYPE cortrix_memory_opt_out_revoke_total counter"), std::string::npos);
    EXPECT_NE(out.find("# TYPE cortrix_memory_opt_out_extract_skipped_total counter"), std::string::npos);

    // Labeled opt_out series (one line per triggered_by enum value).
    EXPECT_NE(out.find("cortrix_memory_opt_out_total{triggered_by=\"user\"} 1"), std::string::npos);
    EXPECT_NE(out.find("cortrix_memory_opt_out_total{triggered_by=\"agent\"} 1"), std::string::npos);
    EXPECT_NE(out.find("cortrix_memory_opt_out_total{triggered_by=\"system\"} 0"), std::string::npos);

    // Unlabeled counters.
    EXPECT_NE(out.find("cortrix_memory_opt_out_revoke_total 1"), std::string::npos);
    EXPECT_NE(out.find("cortrix_memory_opt_out_extract_skipped_total 1"), std::string::npos);
}

TEST(MemoryOptOutMetricsToStringTest, TriggeredByStrings) {
    EXPECT_STREQ(ToString(TB::kUser), "user");
    EXPECT_STREQ(ToString(TB::kAgent), "agent");
    EXPECT_STREQ(ToString(TB::kSystem), "system");
}

}  // namespace
}  // namespace cortrix::memory::immunity
