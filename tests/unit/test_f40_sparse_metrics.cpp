#include <gtest/gtest.h>

#include <string>

#include "cortrix/retrieval/sparse_metrics.h"

// F40 S10 (metrics) — the 4 bge_m3_sparse subsystem metrics. Recorder
// correctness + OpenMetrics rendering + label naming (no ns_id, D1 V3 ruling 10).
namespace cortrix::retrieval {
namespace {

using IS = SparseMetrics::InferenceStatus;
using VP = SparseMetrics::ViaPath;

class F40SparseMetricsTest : public ::testing::Test {
protected:
    void SetUp() override { SparseMetrics::Instance().ResetForTest(); }
    void TearDown() override { SparseMetrics::Instance().ResetForTest(); }
    SparseMetrics& M() { return SparseMetrics::Instance(); }
};

TEST_F(F40SparseMetricsTest, InferenceCounterPerStatus) {
    M().RecordInference(IS::kSuccess);
    M().RecordInference(IS::kSuccess);
    M().RecordInference(IS::kFailed);
    M().RecordInference(IS::kFallback);
    EXPECT_EQ(M().InferenceCount(IS::kSuccess), 2u);
    EXPECT_EQ(M().InferenceCount(IS::kFailed), 1u);
    EXPECT_EQ(M().InferenceCount(IS::kFallback), 1u);
}

TEST_F(F40SparseMetricsTest, FallbackRatioGauge) {
    M().SetFallbackRatio(0.123);
    EXPECT_DOUBLE_EQ(M().FallbackRatio(), 0.123);
    M().SetFallbackRatio(0.5);  // last-write-wins
    EXPECT_DOUBLE_EQ(M().FallbackRatio(), 0.5);
}

TEST_F(F40SparseMetricsTest, InvertedIndexSizeGauge) {
    M().SetInvertedIndexSizeBytes(602u * 1000u);
    EXPECT_EQ(M().InvertedIndexSizeBytes(), 602000u);
}

TEST_F(F40SparseMetricsTest, QueryViaPathCounter) {
    M().RecordQueryViaPath(VP::kSparse);
    M().RecordQueryViaPath(VP::kSparse);
    M().RecordQueryViaPath(VP::kFallbackDenseFts5);
    EXPECT_EQ(M().QueryViaPathCount(VP::kSparse), 2u);
    EXPECT_EQ(M().QueryViaPathCount(VP::kFallbackDenseFts5), 1u);
}

TEST_F(F40SparseMetricsTest, LabelStrings) {
    EXPECT_STREQ(ToString(IS::kSuccess), "success");
    EXPECT_STREQ(ToString(IS::kFailed), "failed");
    EXPECT_STREQ(ToString(IS::kFallback), "fallback");
    EXPECT_STREQ(ToString(VP::kSparse), "sparse");
    EXPECT_STREQ(ToString(VP::kFallbackDenseFts5), "fallback_dense_fts5");
}

TEST_F(F40SparseMetricsTest, RenderContainsAll4Metrics) {
    M().RecordInference(IS::kSuccess);
    M().RecordQueryViaPath(VP::kSparse);
    M().SetFallbackRatio(0.25);
    M().SetInvertedIndexSizeBytes(1234);
    std::string out = M().RenderOpenMetrics();

    // All 4 §10 metric names present + cortrix_ prefix + no ns_id label.
    EXPECT_NE(out.find("cortrix_bge_m3_sparse_inference_total"), std::string::npos);
    EXPECT_NE(out.find("cortrix_bge_m3_sparse_fallback_ratio"), std::string::npos);
    EXPECT_NE(out.find("cortrix_bge_m3_sparse_inverted_index_size_bytes"),
              std::string::npos);
    EXPECT_NE(out.find("cortrix_bge_m3_sparse_query_via_path_total"),
              std::string::npos);
    EXPECT_EQ(out.find("ns_id"), std::string::npos);  // D1 V3 ruling 10

    // TYPE lines + a labeled sample.
    EXPECT_NE(out.find("# TYPE cortrix_bge_m3_sparse_inference_total counter"),
              std::string::npos);
    EXPECT_NE(out.find("status=\"success\""), std::string::npos);
    EXPECT_NE(out.find("path=\"sparse\""), std::string::npos);
}

TEST_F(F40SparseMetricsTest, ResetForTestClearsAll) {
    M().RecordInference(IS::kSuccess);
    M().RecordQueryViaPath(VP::kSparse);
    M().SetFallbackRatio(0.9);
    M().SetInvertedIndexSizeBytes(999);
    M().ResetForTest();
    EXPECT_EQ(M().InferenceCount(IS::kSuccess), 0u);
    EXPECT_EQ(M().QueryViaPathCount(VP::kSparse), 0u);
    EXPECT_DOUBLE_EQ(M().FallbackRatio(), 0.0);
    EXPECT_EQ(M().InvertedIndexSizeBytes(), 0u);
}

}  // namespace
}  // namespace cortrix::retrieval
