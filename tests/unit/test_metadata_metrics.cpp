#include <gtest/gtest.h>

#include <string>

#include "cortrix/metadata/metadata_metrics.h"

// F08 §5.bis coverage: MetadataMetrics (OBSERVABILITY_SPEC subsystem `f08`) — the
// metadata_block lifecycle counters/gauges/histograms F08 drives standalone, and the
// OpenMetrics Render() naming. Mirrors tests/unit/test_import_metrics.cpp style.
namespace cortrix::metadata {
namespace {

using GenStatus = MetadataMetrics::GenStatus;
using GenSource = MetadataMetrics::GenSource;

class MetadataMetricsTest : public ::testing::Test {
protected:
    void SetUp() override { MetadataMetrics::Instance().ResetForTest(); }
    void TearDown() override { MetadataMetrics::Instance().ResetForTest(); }
};

TEST_F(MetadataMetricsTest, GeneratedCounterByStatusAndSource) {
    auto& m = MetadataMetrics::Instance();
    m.RecordGenerated(GenStatus::kSuccess, GenSource::kApiUpload);
    m.RecordGenerated(GenStatus::kSuccess, GenSource::kApiUpload);
    m.RecordGenerated(GenStatus::kPartialWarning, GenSource::kReUpload);
    m.RecordGenerated(GenStatus::kFailed, GenSource::kBatchImport);

    EXPECT_EQ(m.GeneratedCount(GenStatus::kSuccess, GenSource::kApiUpload), 2u);
    EXPECT_EQ(m.GeneratedCount(GenStatus::kPartialWarning, GenSource::kReUpload), 1u);
    EXPECT_EQ(m.GeneratedCount(GenStatus::kFailed, GenSource::kBatchImport), 1u);
    EXPECT_EQ(m.GeneratedCount(GenStatus::kSuccess, GenSource::kReUpload), 0u);
}

TEST_F(MetadataMetricsTest, BlockCountGauge) {
    auto& m = MetadataMetrics::Instance();
    EXPECT_EQ(m.BlockCount(), 0);
    m.SetBlockCount(42);
    EXPECT_EQ(m.BlockCount(), 42);
    m.SetBlockCount(7);
    EXPECT_EQ(m.BlockCount(), 7);
}

TEST_F(MetadataMetricsTest, FieldMissingCounterKeyedByField) {
    auto& m = MetadataMetrics::Instance();
    m.RecordFieldMissing("page_count");
    m.RecordFieldMissing("page_count");
    m.RecordFieldMissing("chunk_count");
    EXPECT_EQ(m.FieldMissingCount("page_count"), 2u);
    EXPECT_EQ(m.FieldMissingCount("chunk_count"), 1u);
    EXPECT_EQ(m.FieldMissingCount("never_seen"), 0u);
}

TEST_F(MetadataMetricsTest, RenderEmitsStableMetricNames) {
    auto& m = MetadataMetrics::Instance();
    m.RecordGenerated(GenStatus::kSuccess, GenSource::kApiUpload);
    m.SetBlockCount(3);
    m.RecordFieldMissing("page_count");
    m.ObserveMetadataSize(1500);
    m.ObserveGenerateDuration(0.012);

    std::string out = m.Render();
    EXPECT_NE(out.find("cortrix_f08_block_count_current 3"), std::string::npos);
    EXPECT_NE(out.find("cortrix_f08_block_generated_total{status=\"success\",source=\"api_upload\"} 1"),
              std::string::npos);
    EXPECT_NE(out.find("cortrix_f08_field_missing_total{field_name=\"page_count\"} 1"),
              std::string::npos);
    EXPECT_NE(out.find("cortrix_f08_metadata_size_bytes_bucket{le=\"2048\"}"), std::string::npos);
    EXPECT_NE(out.find("cortrix_f08_block_generate_duration_seconds_count 1"), std::string::npos);
    // HELP/TYPE lines present (Prometheus scrape contract).
    EXPECT_NE(out.find("# TYPE cortrix_f08_block_generated_total counter"), std::string::npos);
    EXPECT_NE(out.find("# TYPE cortrix_f08_block_count_current gauge"), std::string::npos);

    // High-cardinality labels are forbidden (OBS_SPEC §3.2) — no doc_id / ns_id labels.
    EXPECT_EQ(out.find("doc_id=\""), std::string::npos);
    EXPECT_EQ(out.find("ns_id=\""), std::string::npos);
}

TEST_F(MetadataMetricsTest, HistogramSumAndCountTrack) {
    auto& m = MetadataMetrics::Instance();
    m.ObserveGenerateDuration(0.004);
    m.ObserveGenerateDuration(0.006);
    std::string out = m.Render();
    // 2 observations, smallest bucket le=0.005 catches the 0.004 one.
    EXPECT_NE(out.find("cortrix_f08_block_generate_duration_seconds_count 2"), std::string::npos);
    EXPECT_NE(out.find("cortrix_f08_block_generate_duration_seconds_bucket{le=\"0.005\"} 1"),
              std::string::npos);
}

const char* ToStr(GenStatus s) { return ToString(s); }

TEST_F(MetadataMetricsTest, EnumToStringMapping) {
    EXPECT_STREQ(ToStr(GenStatus::kSuccess), "success");
    EXPECT_STREQ(ToStr(GenStatus::kPartialWarning), "partial_warning");
    EXPECT_STREQ(ToStr(GenStatus::kFailed), "failed");
    EXPECT_STREQ(ToString(GenSource::kApiUpload), "api_upload");
    EXPECT_STREQ(ToString(GenSource::kBatchImport), "batch_import");
    EXPECT_STREQ(ToString(GenSource::kReUpload), "re_upload");
}

}  // namespace
}  // namespace cortrix::metadata
