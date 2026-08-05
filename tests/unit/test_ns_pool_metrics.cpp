#include <gtest/gtest.h>

#include <string>

#include "cortrix/resource/ns_pool_metrics.h"

// MET-12 coverage: the 6 cortrix_ns_pool_* metrics — gauges/counters/
// histograms recording + the OpenMetrics renderer + label-enum discipline
// (OBS_SPEC §3.2 no high-cardinality labels; the namespace_id lives only in the
// explain/stats API JSON, never a metric label).
namespace cortrix::resource {
namespace {

using RejectReason = NsPoolMetrics::RejectReason;

class NsPoolMetricsTest : public ::testing::Test {
protected:
    void SetUp() override { NsPoolMetrics::Instance().ResetForTest(); }
    void TearDown() override { NsPoolMetrics::Instance().ResetForTest(); }
    NsPoolMetrics& M() { return NsPoolMetrics::Instance(); }
};

TEST_F(NsPoolMetricsTest, SizeAndMemoryGaugesSetAndClamped) {
    M().SetSize(7);
    M().SetMemoryBudgetUsedBytes(8589934592);  // 8 GiB
    EXPECT_EQ(M().Size(), 7);
    EXPECT_EQ(M().MemoryBudgetUsedBytes(), 8589934592);
    M().SetSize(-1);                    // clamped to 0
    M().SetMemoryBudgetUsedBytes(-5);   // clamped to 0
    EXPECT_EQ(M().Size(), 0);
    EXPECT_EQ(M().MemoryBudgetUsedBytes(), 0);
}

TEST_F(NsPoolMetricsTest, RejectedCreatesCountsByReason) {
    M().RecordRejectedCreate(RejectReason::kNsCountExceeded);
    M().RecordRejectedCreate(RejectReason::kNsCountExceeded);
    M().RecordRejectedCreate(RejectReason::kMemoryExceeded);
    EXPECT_EQ(M().RejectedCreateCount(RejectReason::kNsCountExceeded), 2u);
    EXPECT_EQ(M().RejectedCreateCount(RejectReason::kMemoryExceeded), 1u);
}

TEST_F(NsPoolMetricsTest, StartupLoadFailuresAccumulate) {
    M().AddStartupLoadFailures(1);
    M().AddStartupLoadFailures(2);
    EXPECT_EQ(M().StartupLoadFailuresCount(), 3u);
}

TEST_F(NsPoolMetricsTest, RenderOpenMetricsHasAllSixMetricsAndTypes) {
    M().SetSize(5);
    M().SetMemoryBudgetUsedBytes(1024);
    M().RecordRejectedCreate(RejectReason::kMemoryExceeded);
    M().AddStartupLoadFailures(1);
    M().ObserveStartupLoadDuration(4.0);
    M().ObserveNsLoadDuration(2.5);

    std::string out = M().RenderOpenMetrics();
    // All 6 metric names present.
    EXPECT_NE(out.find("cortrix_ns_pool_size"), std::string::npos);
    EXPECT_NE(out.find("cortrix_ns_pool_memory_budget_used_bytes"), std::string::npos);
    EXPECT_NE(out.find("cortrix_ns_pool_rejected_creates_total"), std::string::npos);
    EXPECT_NE(out.find("cortrix_ns_pool_startup_load_failures_total"), std::string::npos);
    EXPECT_NE(out.find("cortrix_ns_pool_startup_load_duration_seconds"), std::string::npos);
    EXPECT_NE(out.find("cortrix_ns_pool_ns_load_duration_seconds"), std::string::npos);
    // TYPE lines (gauge / counter / histogram).
    EXPECT_NE(out.find("# TYPE cortrix_ns_pool_size gauge"), std::string::npos);
    EXPECT_NE(out.find("# TYPE cortrix_ns_pool_rejected_creates_total counter"), std::string::npos);
    EXPECT_NE(out.find("# TYPE cortrix_ns_pool_startup_load_duration_seconds histogram"),
              std::string::npos);
    // Reason label appears with the spec-exact value.
    EXPECT_NE(out.find("cortrix_ns_pool_rejected_creates_total{reason=\"memory_exceeded\"}"),
              std::string::npos);
}

// Histogram renders well-formed cumulative buckets: every declared le bound + a
// +Inf bucket, monotonically non-decreasing, with +Inf == _count.
TEST_F(NsPoolMetricsTest, NsLoadDurationHistogramBucketsAreCumulativeAndComplete) {
    // Observations across buckets (s): 0.05→le 0.1, 2.5→le 3, 8→le 10, 120→+Inf only.
    M().ObserveNsLoadDuration(0.05);
    M().ObserveNsLoadDuration(2.5);
    M().ObserveNsLoadDuration(8.0);
    M().ObserveNsLoadDuration(120.0);
    std::string out = M().RenderOpenMetrics();

    const std::string prefix = "cortrix_ns_pool_ns_load_duration_seconds_bucket{le=\"";
    auto bucket_value = [&](const std::string& le) -> long {
        std::string key = prefix + le + "\"} ";
        size_t p = out.find(key);
        if (p == std::string::npos) return -1;
        p += key.size();
        return std::stol(out.substr(p, out.find('\n', p) - p));
    };

    long prev = 0;
    for (const char* le : {"0.1", "0.5", "1", "3", "5", "10", "30", "60"}) {
        long v = bucket_value(le);
        ASSERT_GE(v, 0) << "missing bucket le=" << le;
        EXPECT_GE(v, prev) << "bucket le=" << le << " not cumulative";
        prev = v;
    }
    EXPECT_EQ(bucket_value("0.1"), 1);   // 0.05s
    EXPECT_EQ(bucket_value("3"), 2);     // + 2.5s
    EXPECT_EQ(bucket_value("10"), 3);    // + 8s
    EXPECT_EQ(bucket_value("60"), 3);    // 120s only in +Inf
    EXPECT_EQ(bucket_value("+Inf"), 4);  // +Inf == total
    EXPECT_NE(out.find("cortrix_ns_pool_ns_load_duration_seconds_count 4"), std::string::npos);
}

TEST_F(NsPoolMetricsTest, RenderHasNoHighCardinalityLabels) {
    // OBS_SPEC §3.2 / F05 §10.1: no namespace / unit_id / tenant_id labels.
    M().SetSize(1);
    M().RecordRejectedCreate(RejectReason::kNsCountExceeded);
    std::string out = M().RenderOpenMetrics();
    EXPECT_EQ(out.find("namespace_id"), std::string::npos);
    EXPECT_EQ(out.find("unit_id"), std::string::npos);
    EXPECT_EQ(out.find("tenant_id"), std::string::npos);
}

TEST_F(NsPoolMetricsTest, LabelStringsMatchSpec) {
    // Must match PoolStats.rejected_creates_total field names + RejectionEvent.reason.
    EXPECT_STREQ(ToString(RejectReason::kNsCountExceeded), "ns_count_exceeded");
    EXPECT_STREQ(ToString(RejectReason::kMemoryExceeded), "memory_exceeded");
}

}  // namespace
}  // namespace cortrix::resource
