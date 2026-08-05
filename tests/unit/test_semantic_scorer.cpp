#include <gtest/gtest.h>

#include <cstring>
#include <optional>

#include "cortrix/agent_friendly/error.h"
#include "cortrix/common/block_header.h"
#include "cortrix/common/block_types.h"
#include "cortrix/scoring/score_map.h"
#include "cortrix/scoring/scoring_metrics.h"
#include "cortrix/scoring/semantic_scorer.h"

// Semantic score S3 + S4 / §7.1 coverage: SemanticScorer.AssignInitialScore (D7 writes to both places + D6
// sentinel) + ComputeFinalScore (D5 multiplier formula). Standalone = pure algorithm over constructed
// cortrix_block_header_t fixtures (B_R3_BRIEFING §2/§7).
namespace cortrix::scoring {
namespace {

ScoringInput Input(const std::string& parser, const std::string& enricher,
                   bool anomalous = false, uint16_t block_type = kBlockFile) {
    ScoringInput in;
    in.parser_name = parser;
    in.enricher_name = enricher;
    in.is_anomalous = anomalous;
    in.block_type = block_type;
    return in;
}

// A zeroed, initialized header fixture (the block header init path).
cortrix::cortrix_block_header_t Header() {
    cortrix::cortrix_block_header_t h;
    cortrix::BlockHeaderInit(&h);
    return h;
}

// --- AssignInitialScore (D7 dual-write + D6 sentinel) ---

TEST(SemanticScorerAssignTest, WritesBothHeaderLevelAndScore) {
    SemanticScorer scorer;
    auto h = Header();
    float score = -1.0f;
    // docling + llm → Level 4 → score 1.0.
    scorer.AssignInitialScore(h, score, Input("docling", "llm"));
    EXPECT_EQ(h.processing_level, 4);     // D7: header byte (immutable) written
    EXPECT_FLOAT_EQ(score, 1.0f);          // D7: blocks-column score written
}

TEST(SemanticScorerAssignTest, LevelScoreConsistencyAcrossLevels) {
    SemanticScorer scorer;
    struct Case { const char* parser; const char* enricher; uint8_t level; float score; };
    const Case cases[] = {
        {"", "", 0, 0.2f},
        {"paddleocr", "null", 1, 0.4f},
        {"docling", "null", 2, 0.6f},
        {"docling", "contextual", 3, 0.8f},
        {"docling", "llm", 4, 1.0f},
    };
    for (const auto& c : cases) {
        auto h = Header();
        float score = -1.0f;
        scorer.AssignInitialScore(h, score, Input(c.parser, c.enricher));
        EXPECT_EQ(h.processing_level, c.level) << c.parser << "+" << c.enricher;
        EXPECT_FLOAT_EQ(score, c.score) << c.parser << "+" << c.enricher;
    }
}

// D6: anomalous → score 0.0 sentinel, but processing_level keeps the real computed value.
TEST(SemanticScorerAssignTest, AnomalousGetsSentinelZeroButRealLevel) {
    SemanticScorer scorer;
    auto h = Header();
    float score = -1.0f;
    // docling+llm would be Level 4 / 1.0; anomalous forces score 0.0 but level stays 4.
    scorer.AssignInitialScore(h, score, Input("docling", "llm", /*anomalous=*/true));
    EXPECT_FLOAT_EQ(score, 0.0f);     // D6 sentinel
    EXPECT_EQ(h.processing_level, 4);  // level preserved (actual processing depth)
}

// Meta Block (block_type 8) → Level 0 / 0.2 via ComputeLevel special case (M1).
TEST(SemanticScorerAssignTest, MetaBlockScoresFloor) {
    SemanticScorer scorer;
    auto h = Header();
    float score = -1.0f;
    scorer.AssignInitialScore(h, score, Input("docling", "llm", false, kBlockMeta));
    EXPECT_EQ(h.processing_level, 0);
    EXPECT_FLOAT_EQ(score, 0.2f);
}

// Re-assigning overwrites prior values (idempotent re-scoring).
TEST(SemanticScorerAssignTest, ReassignOverwrites) {
    SemanticScorer scorer;
    auto h = Header();
    float score = -1.0f;
    scorer.AssignInitialScore(h, score, Input("docling", "llm"));
    EXPECT_FLOAT_EQ(score, 1.0f);
    scorer.AssignInitialScore(h, score, Input("paddleocr", "null"));
    EXPECT_FLOAT_EQ(score, 0.4f);
    EXPECT_EQ(h.processing_level, 1);
}

// QA regression: AssignInitialScore must feed the §6 assign_duration histogram
// (it was defined/rendered but never observed). Lock the wiring: each call increments
// cortrix_f07_assign_duration_seconds_count by 1.
TEST(SemanticScorerAssignTest, AssignFeedsDurationHistogram) {
    ScoringMetrics::Instance().ResetForTest();
    SemanticScorer scorer;
    auto h = Header();
    float score = -1.0f;
    scorer.AssignInitialScore(h, score, Input("docling", "llm"));
    scorer.AssignInitialScore(h, score, Input("paddleocr", "null"));

    const std::string out = ScoringMetrics::Instance().Render();
    EXPECT_NE(out.find("cortrix_f07_assign_duration_seconds_count 2"), std::string::npos)
        << out;
    ScoringMetrics::Instance().ResetForTest();
}

// --- ComputeFinalScore (D5 multiplier formula) ---

TEST(SemanticScorerFinalTest, FourKeyEffectivePoints) {
    const float rerank = 1.0f;
    // effective=0.5 → ×1.0 (unchanged).
    EXPECT_NEAR(SemanticScorer::ComputeFinalScore(rerank, std::nullopt, 0.5f), 1.0f, 1e-5);
    // effective=1.0 → ×1.1 (+10%).
    EXPECT_NEAR(SemanticScorer::ComputeFinalScore(rerank, std::nullopt, 1.0f), 1.1f, 1e-5);
    // effective=0.2 → ×0.94 (-6%).
    EXPECT_NEAR(SemanticScorer::ComputeFinalScore(rerank, std::nullopt, 0.2f), 0.94f, 1e-5);
    // effective=0.0 (anomalous) → ×0.9 (-10%).
    EXPECT_NEAR(SemanticScorer::ComputeFinalScore(rerank, std::nullopt, 0.6f,
                                                  /*is_anomalous=*/true), 0.9f, 1e-5);
}

// enriched_score (non-null) takes precedence over semantic_score (coalesce).
TEST(SemanticScorerFinalTest, EnrichedScoreCoalescesOverSemantic) {
    // enriched=0.8 used (not semantic=0.2): final = 1.0 × (1 + 0.2×(0.8-0.5)) = 1.06.
    EXPECT_NEAR(SemanticScorer::ComputeFinalScore(1.0f, 0.8f, 0.2f), 1.06f, 1e-5);
}

// (v1.0.1 M2) anomalous overrides even a present enriched_score → effective 0.0.
TEST(SemanticScorerFinalTest, AnomalousOverridesEnriched) {
    // enriched=0.8 present but anomalous → effective 0.0 → ×0.9.
    EXPECT_NEAR(SemanticScorer::ComputeFinalScore(1.0f, 0.8f, 0.6f, /*is_anomalous=*/true),
                0.9f, 1e-5);
}

// rerank=0 edge → final 0 regardless of effective.
TEST(SemanticScorerFinalTest, ZeroRerankStaysZero) {
    EXPECT_FLOAT_EQ(SemanticScorer::ComputeFinalScore(0.0f, std::nullopt, 1.0f), 0.0f);
}

// α scales the adjustment magnitude; α=0 → final == rerank.
TEST(SemanticScorerFinalTest, AlphaScalesAdjustment) {
    EXPECT_NEAR(SemanticScorer::ComputeFinalScore(1.0f, std::nullopt, 1.0f, false, /*alpha=*/0.0f),
                1.0f, 1e-5);
    // α=0.5, effective=1.0 → ×(1 + 0.5×0.5) = 1.25.
    EXPECT_NEAR(SemanticScorer::ComputeFinalScore(1.0f, std::nullopt, 1.0f, false, 0.5f),
                1.25f, 1e-5);
    // boundary α=1.0 valid.
    EXPECT_NO_THROW(SemanticScorer::ComputeFinalScore(1.0f, std::nullopt, 1.0f, false, 1.0f));
}

TEST(SemanticScorerFinalTest, AlphaOutOfRangeThrowsConfigInvalid) {
    try {
        SemanticScorer::ComputeFinalScore(1.0f, std::nullopt, 0.5f, false, /*alpha=*/1.5f);
        FAIL() << "expected AgentFriendlyException";
    } catch (const agent_friendly::AgentFriendlyException& e) {
        EXPECT_EQ(e.GetError().code, "CX_ERR_SCORING_CONFIG_INVALID");
        ASSERT_TRUE(e.GetError().structured_data.has_value());
        EXPECT_EQ((*e.GetError().structured_data)["config_source"], "config.yaml:scoring.alpha");
    }
    EXPECT_THROW(SemanticScorer::ComputeFinalScore(1.0f, std::nullopt, 0.5f, false, -0.1f),
                 agent_friendly::AgentFriendlyException);
}

}  // namespace
}  // namespace cortrix::scoring
