#include <gtest/gtest.h>

#include <string>

#include "cortrix/observability/oplog_metrics.h"

// F18a §11 OBS_SPEC metrics recorder — the 6 cortrix_oplog_* series. Standalone
// recorder coverage: registration (all 6 names render), record/aggregate, the
// {action,resource_type} free-string keys, the filter_dimensions + reason labels,
// and the two histograms.
namespace cortrix::observability {
namespace {

// The process-wide singleton is shared; reset before each test for isolation.
class OplogMetricsTest : public ::testing::Test {
protected:
    void SetUp() override { OplogMetrics::Instance().ResetForTest(); }
    void TearDown() override { OplogMetrics::Instance().ResetForTest(); }
    OplogMetrics& M() { return OplogMetrics::Instance(); }
};

// All 6 §11 metrics are present in the OpenMetrics exposition (registration proof).
TEST_F(OplogMetricsTest, AllSixMetricsRegistered) {
    // Drive at least one observation into each so labeled/histogram series render.
    M().RecordWrite("namespace_create", "namespace");
    M().ObserveQueryLatency(/*filter_dimensions=*/2, /*latency_ms=*/12);
    M().RecordCleanupDeleted(OplogMetrics::CleanupReason::kAge, 3);
    M().ObserveCleanupDuration(40);
    M().RecordCleanupFailed();
    M().SetSizeRows(99);

    const std::string out = M().RenderOpenMetrics();
    EXPECT_NE(out.find("cortrix_oplog_writes_total"), std::string::npos);
    EXPECT_NE(out.find("cortrix_oplog_query_latency_seconds"), std::string::npos);
    EXPECT_NE(out.find("cortrix_oplog_cleanup_deleted_total"), std::string::npos);
    EXPECT_NE(out.find("cortrix_oplog_cleanup_duration_seconds"), std::string::npos);
    EXPECT_NE(out.find("cortrix_oplog_cleanup_failed_total"), std::string::npos);
    EXPECT_NE(out.find("cortrix_oplog_size_rows"), std::string::npos);

    // Each declares its TYPE (counter/histogram/gauge) per OpenMetrics.
    EXPECT_NE(out.find("# TYPE cortrix_oplog_writes_total counter"), std::string::npos);
    EXPECT_NE(out.find("# TYPE cortrix_oplog_query_latency_seconds histogram"), std::string::npos);
    EXPECT_NE(out.find("# TYPE cortrix_oplog_cleanup_duration_seconds histogram"), std::string::npos);
    EXPECT_NE(out.find("# TYPE cortrix_oplog_size_rows gauge"), std::string::npos);
}

// writes_total keys on the {action, resource_type} pair (free-string action).
TEST_F(OplogMetricsTest, WritesTotalKeysByActionAndResourceType) {
    M().RecordWrite("query", "query");
    M().RecordWrite("query", "query");
    M().RecordWrite("namespace_create", "namespace");
    M().RecordWrite("database_import", "db_import");  // 6-value resource_type domain

    EXPECT_EQ(M().WriteCount("query", "query"), 2u);
    EXPECT_EQ(M().WriteCount("namespace_create", "namespace"), 1u);
    EXPECT_EQ(M().WriteCount("database_import", "db_import"), 1u);
    EXPECT_EQ(M().WriteCount("never", "seen"), 0u);

    const std::string out = M().RenderOpenMetrics();
    EXPECT_NE(out.find("cortrix_oplog_writes_total{action=\"query\",resource_type=\"query\"} 2"),
              std::string::npos);
    EXPECT_NE(out.find("cortrix_oplog_writes_total{action=\"database_import\",resource_type=\"db_import\"} 1"),
              std::string::npos);
}

// query_latency histogram is per filter_dimensions; counts isolate by dimension.
TEST_F(OplogMetricsTest, QueryLatencyHistogramPerFilterDimension) {
    M().ObserveQueryLatency(0, 5);
    M().ObserveQueryLatency(3, 8);
    M().ObserveQueryLatency(3, 900);
    EXPECT_EQ(M().QueryLatencyCount(0), 1u);
    EXPECT_EQ(M().QueryLatencyCount(3), 2u);
    EXPECT_EQ(M().QueryLatencyCount(5), 0u);

    const std::string out = M().RenderOpenMetrics();
    EXPECT_NE(out.find("filter_dimensions=\"3\""), std::string::npos);
    EXPECT_NE(out.find("cortrix_oplog_query_latency_seconds_count{filter_dimensions=\"3\"} 2"),
              std::string::npos);
}

// filter_dimensions is clamped into [0,8] (out-of-range folds into the 8 series).
TEST_F(OplogMetricsTest, FilterDimensionsClamped) {
    M().ObserveQueryLatency(99, 10);   // > 8 → series 8
    M().ObserveQueryLatency(-4, 10);   // < 0 → series 0
    EXPECT_EQ(M().QueryLatencyCount(8), 1u);
    EXPECT_EQ(M().QueryLatencyCount(0), 1u);
}

// cleanup_deleted_total splits by reason; zero/negative counts are ignored.
TEST_F(OplogMetricsTest, CleanupDeletedSplitsByReason) {
    M().RecordCleanupDeleted(OplogMetrics::CleanupReason::kAge, 5);
    M().RecordCleanupDeleted(OplogMetrics::CleanupReason::kAge, 2);
    M().RecordCleanupDeleted(OplogMetrics::CleanupReason::kQuota, 4);
    M().RecordCleanupDeleted(OplogMetrics::CleanupReason::kQuota, 0);   // no-op
    M().RecordCleanupDeleted(OplogMetrics::CleanupReason::kAge, -1);    // no-op

    EXPECT_EQ(M().CleanupDeletedCount(OplogMetrics::CleanupReason::kAge), 7u);
    EXPECT_EQ(M().CleanupDeletedCount(OplogMetrics::CleanupReason::kQuota), 4u);

    const std::string out = M().RenderOpenMetrics();
    EXPECT_NE(out.find("cortrix_oplog_cleanup_deleted_total{reason=\"age\"} 7"), std::string::npos);
    EXPECT_NE(out.find("cortrix_oplog_cleanup_deleted_total{reason=\"quota\"} 4"), std::string::npos);
}

// size_rows is a gauge — last write wins (not monotonic).
TEST_F(OplogMetricsTest, SizeRowsGaugeLastWriteWins) {
    M().SetSizeRows(100);
    M().SetSizeRows(42);
    EXPECT_EQ(M().SizeRows(), 42);
    const std::string out = M().RenderOpenMetrics();
    EXPECT_NE(out.find("cortrix_oplog_size_rows 42"), std::string::npos);
}

// cleanup_failed_total is a monotonic counter.
TEST_F(OplogMetricsTest, CleanupFailedCounter) {
    M().RecordCleanupFailed();
    M().RecordCleanupFailed();
    EXPECT_EQ(M().CleanupFailedCount(), 2u);
    const std::string out = M().RenderOpenMetrics();
    EXPECT_NE(out.find("cortrix_oplog_cleanup_failed_total 2"), std::string::npos);
}

}  // namespace
}  // namespace cortrix::observability
