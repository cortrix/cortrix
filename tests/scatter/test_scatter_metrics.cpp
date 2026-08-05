#include <gtest/gtest.h>

#include <string>

#include "cortrix/query/scatter_metrics.h"

// S3.4 coverage: the `scatter` subsystem metric recorder + OpenMetrics renderer.
namespace cortrix::query {
namespace {

using agent_friendly::ErrorCategory;
using Reason = ScatterMetrics::Reason;

class ScatterMetricsTest : public ::testing::Test {
 protected:
  void SetUp() override { ScatterMetrics::Instance().ResetForTest(); }
  void TearDown() override { ScatterMetrics::Instance().ResetForTest(); }
  ScatterMetrics& m = ScatterMetrics::Instance();
};

TEST_F(ScatterMetricsTest, RequestCounterByReasonAndCategory) {
    m.RecordRequest(Reason::kSingle, ErrorCategory::kPermanent);
    m.RecordRequest(Reason::kMulti, ErrorCategory::kPermanent);
    m.RecordRequest(Reason::kMulti, ErrorCategory::kPermanent);
    EXPECT_EQ(m.RequestCount(Reason::kSingle, ErrorCategory::kPermanent), 1u);
    EXPECT_EQ(m.RequestCount(Reason::kMulti, ErrorCategory::kPermanent), 2u);
    EXPECT_EQ(m.RequestCount(Reason::kWildcard, ErrorCategory::kPermanent), 0u);
}

TEST_F(ScatterMetricsTest, FailedCounterIsSeparate) {
    m.RecordFailed(Reason::kMulti, ErrorCategory::kAuth);
    EXPECT_EQ(m.FailedCount(Reason::kMulti, ErrorCategory::kAuth), 1u);
    EXPECT_EQ(m.RequestCount(Reason::kMulti, ErrorCategory::kAuth), 0u);
}

TEST_F(ScatterMetricsTest, NamespacesPerQueryHistogram) {
    m.ObserveNamespacesPerQuery(3);
    m.ObserveNamespacesPerQuery(7);
    EXPECT_EQ(m.NamespacesPerQueryCount(), 2u);
    EXPECT_EQ(m.NamespacesPerQuerySum(), 10u);
}

TEST_F(ScatterMetricsTest, DedupCollisionsAccumulate) {
    m.RecordDedupCollisions(2);
    m.RecordDedupCollisions(3);
    EXPECT_EQ(m.DedupCollisionsCount(), 5u);
}

TEST_F(ScatterMetricsTest, PartialSuccessCounter) {
    m.RecordPartialSuccess();
    m.RecordPartialSuccess();
    EXPECT_EQ(m.PartialSuccessCount(), 2u);
}

TEST_F(ScatterMetricsTest, DurationBucketsMatchSlaTiers) {
    EXPECT_EQ(ScatterMetrics::DurationBucket(1), 0);
    EXPECT_EQ(ScatterMetrics::DurationBucket(3), 1);
    EXPECT_EQ(ScatterMetrics::DurationBucket(10), 2);
    EXPECT_EQ(ScatterMetrics::DurationBucket(100), 3);
    EXPECT_EQ(ScatterMetrics::DurationBucket(50), 3);
}

TEST_F(ScatterMetricsTest, DurationObservationsBucketed) {
    m.ObserveDuration(1, 100);    // bucket 0
    m.ObserveDuration(10, 800);   // bucket 2
    m.ObserveDuration(10, 200);   // bucket 2
    EXPECT_EQ(m.DurationCount(0), 1u);
    EXPECT_EQ(m.DurationSumMs(0), 100u);
    EXPECT_EQ(m.DurationCount(2), 2u);
    EXPECT_EQ(m.DurationSumMs(2), 1000u);
}

TEST_F(ScatterMetricsTest, RenderOpenMetricsHasAllSixMetrics) {
    m.RecordRequest(Reason::kSingle, ErrorCategory::kPermanent);
    m.RecordDedupCollisions(1);
    const std::string text = m.RenderOpenMetrics();
    for (const char* name : {"cortrix_scatter_requests_total",
                             "cortrix_scatter_failed_total",
                             "cortrix_scatter_namespaces_per_query",
                             "cortrix_scatter_dedup_collisions_total",
                             "cortrix_scatter_partial_success_total",
                             "cortrix_scatter_duration_seconds"}) {
        EXPECT_NE(text.find(name), std::string::npos) << "missing metric: " << name;
    }
    // reason+category labels present (GEN-Agent).
    EXPECT_NE(text.find("reason=\"single\""), std::string::npos);
    EXPECT_NE(text.find("category=\"permanent\""), std::string::npos);
}

TEST_F(ScatterMetricsTest, ReasonToString) {
    EXPECT_STREQ(ToString(Reason::kSingle), "single");
    EXPECT_STREQ(ToString(Reason::kMulti), "multi");
    EXPECT_STREQ(ToString(Reason::kWildcard), "wildcard");
}

}  // namespace
}  // namespace cortrix::query
