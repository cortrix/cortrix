#include <gtest/gtest.h>

#include <string>

#include "cortrix/scoring/scoring_metrics.h"

// F07 §6 / S9 coverage: ScoringMetrics (OBSERVABILITY_SPEC subsystem `scoring`) — the
// score-by-level / anomalous / final-score counters + assign-duration histogram, and the
// OpenMetrics Render() naming. Mirrors tests/unit/test_import_metrics.cpp style.
namespace cortrix::scoring {
namespace {

class ScoringMetricsTest : public ::testing::Test {
protected:
    void SetUp() override { ScoringMetrics::Instance().ResetForTest(); }
    void TearDown() override { ScoringMetrics::Instance().ResetForTest(); }
};

TEST_F(ScoringMetricsTest, ScoreCounterByLevel) {
    auto& m = ScoringMetrics::Instance();
    m.RecordScore(0);
    m.RecordScore(4);
    m.RecordScore(4);
    EXPECT_EQ(m.ScoreCount(0), 1u);
    EXPECT_EQ(m.ScoreCount(4), 2u);
    EXPECT_EQ(m.ScoreCount(2), 0u);
    // Out-of-range level is ignored (defensive — never recorded).
    m.RecordScore(7);
    EXPECT_EQ(m.ScoreCount(7), 0u);
}

TEST_F(ScoringMetricsTest, AnomalousAndFinalScoreCounters) {
    auto& m = ScoringMetrics::Instance();
    m.RecordAnomalous();
    m.RecordAnomalous();
    m.RecordFinalScore();
    EXPECT_EQ(m.AnomalousCount(), 2u);
    EXPECT_EQ(m.FinalScoreCount(), 1u);
}

TEST_F(ScoringMetricsTest, RenderEmitsStableMetricNames) {
    auto& m = ScoringMetrics::Instance();
    m.RecordScore(2);
    m.RecordAnomalous();
    m.RecordFinalScore();
    m.ObserveAssignDuration(0.0005);

    std::string out = m.Render();
    EXPECT_NE(out.find("cortrix_f07_score_total{level=\"2\"} 1"), std::string::npos);
    EXPECT_NE(out.find("cortrix_f07_score_total{level=\"0\"} 0"), std::string::npos);
    EXPECT_NE(out.find("cortrix_f07_anomalous_blocks_total 1"), std::string::npos);
    EXPECT_NE(out.find("cortrix_f07_final_score_total 1"), std::string::npos);
    EXPECT_NE(out.find("cortrix_f07_assign_duration_seconds_count 1"), std::string::npos);
    EXPECT_NE(out.find("# TYPE cortrix_f07_score_total counter"), std::string::npos);

    // No high-cardinality ns_id label (OBS_SPEC §3.2 / F07 §6 C1 removed it).
    EXPECT_EQ(out.find("ns_id=\""), std::string::npos);
}

TEST_F(ScoringMetricsTest, AllFiveLevelsRendered) {
    std::string out = ScoringMetrics::Instance().Render();
    for (int lv = 0; lv <= 4; ++lv) {
        EXPECT_NE(out.find("level=\"" + std::to_string(lv) + "\""), std::string::npos);
    }
}

}  // namespace
}  // namespace cortrix::scoring
