#include <gtest/gtest.h>

#include <string>

#include "cortrix/deploy/deploy_metrics.h"

// Deployment coverage: the deployment/system gauges (disk_usage_ratio /
// shutdown_status / uptime / build_info) and the read-through Bloom
// Filter gauges — recording + the OpenMetrics text renderer (stable names,
// HELP/TYPE lines, subsystem="catalog" label, epoch-second conversion).
namespace cortrix::deploy {
namespace {

bool Contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

class DeployMetricsTest : public ::testing::Test {
protected:
    void SetUp() override { DeployMetrics::Instance().ResetForTest(); }
    void TearDown() override { DeployMetrics::Instance().ResetForTest(); }
    DeployMetrics& M() { return DeployMetrics::Instance(); }
};

TEST_F(DeployMetricsTest, DiskUsageRatioGaugeClampsAndRoundTrips) {
    M().SetDiskUsageRatio(0.42);
    EXPECT_DOUBLE_EQ(M().DiskUsageRatio(), 0.42);
    M().SetDiskUsageRatio(1.5);   // clamp high
    EXPECT_DOUBLE_EQ(M().DiskUsageRatio(), 1.0);
    M().SetDiskUsageRatio(-0.3);  // clamp low
    EXPECT_DOUBLE_EQ(M().DiskUsageRatio(), 0.0);
}

TEST_F(DeployMetricsTest, ShutdownStatusGaugeClampsToZeroOneTwo) {
    M().SetShutdownStatus(1);
    EXPECT_EQ(M().ShutdownStatus(), 1);
    M().SetShutdownStatus(2);
    EXPECT_EQ(M().ShutdownStatus(), 2);
    M().SetShutdownStatus(9);   // clamp to 2
    EXPECT_EQ(M().ShutdownStatus(), 2);
    M().SetShutdownStatus(-1);  // clamp to 0
    EXPECT_EQ(M().ShutdownStatus(), 0);
}

TEST_F(DeployMetricsTest, RenderEmitsAllFourGaugesWithHelpType) {
    M().SetDiskUsageRatio(0.55);
    M().SetShutdownStatus(1);
    M().SetBuildInfo("1.0.0-rc.1", "abc1234", "2026-06-02");
    M().MarkStart();
    std::string out = M().Render();

    // disk_usage_ratio
    EXPECT_TRUE(Contains(out, "# TYPE cortrix_disk_usage_ratio gauge"));
    EXPECT_TRUE(Contains(out, "cortrix_disk_usage_ratio 0.55"));
    // shutdown_status
    EXPECT_TRUE(Contains(out, "# TYPE cortrix_shutdown_status gauge"));
    EXPECT_TRUE(Contains(out, "cortrix_shutdown_status 1"));
    // uptime
    EXPECT_TRUE(Contains(out, "# TYPE cortrix_uptime_seconds gauge"));
    EXPECT_TRUE(Contains(out, "cortrix_uptime_seconds "));
    // build_info — info gauge value 1 with the three labels
    EXPECT_TRUE(Contains(out, "# TYPE cortrix_build_info gauge"));
    EXPECT_TRUE(Contains(out, "version=\"1.0.0-rc.1\""));
    EXPECT_TRUE(Contains(out, "git_commit=\"abc1234\""));
    EXPECT_TRUE(Contains(out, "build_date=\"2026-06-02\""));
    EXPECT_TRUE(Contains(out, "} 1\n"));  // value is always 1
}

TEST_F(DeployMetricsTest, BuildInfoLabelValuesAreEscaped) {
    M().SetBuildInfo("1.0\"x", "a\\b", "d");
    std::string out = M().Render();
    EXPECT_TRUE(Contains(out, "version=\"1.0\\\"x\""));   // double-quote is escaped
    EXPECT_TRUE(Contains(out, "git_commit=\"a\\\\b\""));  // backslash is doubled
}

TEST(BloomFilterMetricsTest, RendersFourGaugesWithCatalogSubsystem) {
    BloomFilterMetricSource src;
    src.estimated_count = [] { return uint64_t{42000}; };
    src.false_positive_rate = [] { return 0.0085; };
    src.last_rebuild_epoch_sec = [] { return int64_t{1768694400}; };
    src.ready = [] { return true; };
    std::string out = RenderBloomFilterMetrics(src);

    EXPECT_TRUE(Contains(out, "cortrix_bloom_filter_estimated_count{subsystem=\"catalog\"} 42000"));
    EXPECT_TRUE(Contains(out, "cortrix_bloom_filter_false_positive_rate{subsystem=\"catalog\"} 0.0085"));
    EXPECT_TRUE(Contains(out, "cortrix_bloom_filter_last_rebuild_ts{subsystem=\"catalog\"} 1768694400"));
    EXPECT_TRUE(Contains(out, "cortrix_bloom_filter_ready{subsystem=\"catalog\"} 1"));
    // HELP/TYPE present for each
    EXPECT_TRUE(Contains(out, "# TYPE cortrix_bloom_filter_ready gauge"));
}

TEST(BloomFilterMetricsTest, NeverRebuiltEmitsZeroTimestamp) {
    BloomFilterMetricSource src;
    src.last_rebuild_epoch_sec = [] { return int64_t{-1}; };  // never rebuilt
    src.ready = [] { return false; };
    std::string out = RenderBloomFilterMetrics(src);
    EXPECT_TRUE(Contains(out, "cortrix_bloom_filter_last_rebuild_ts{subsystem=\"catalog\"} 0"));
    EXPECT_TRUE(Contains(out, "cortrix_bloom_filter_ready{subsystem=\"catalog\"} 0"));
}

TEST(BloomFilterMetricsTest, NullAccessorsAreSkipped) {
    BloomFilterMetricSource src;
    src.ready = [] { return true; };  // only one bound
    std::string out = RenderBloomFilterMetrics(src);
    EXPECT_TRUE(Contains(out, "cortrix_bloom_filter_ready"));
    EXPECT_FALSE(Contains(out, "cortrix_bloom_filter_estimated_count"));
}

}  // namespace
}  // namespace cortrix::deploy
