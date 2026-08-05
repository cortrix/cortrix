#include <cstdint>
#include "cortrix/scoring/semantic_scorer.h"

#include <chrono>

#include "cortrix/agent_friendly/error.h"
#include "cortrix/scoring/scoring_error.h"
#include "cortrix/scoring/scoring_metrics.h"

namespace cortrix::scoring {

void SemanticScorer::AssignInitialScore(cortrix::cortrix_block_header_t& header,
                                        float& semantic_score,
                                        const ScoringInput& input,
                                        const observability::TraceContext* /*ctx*/) {
    // §6 cortrix_f07_assign_duration_seconds — time the scoring work (steady_clock,
    // immune to wall-clock jumps). QA finding: the histogram was defined but never fed.
    const auto start = std::chrono::steady_clock::now();

    const uint8_t level = ScoreMap::ComputeLevel(input);  // always in [0,4]
    // processing_level is the block header byte (immutable, written here as the sole
    // source of computation). Written even for anomalous blocks (the level reflects the
    // actual processing depth).
    header.processing_level = level;

    // D6: anomalous → 0.0 sentinel (matching D5's formula natural decay final = rerank × 0.9); else the
    // discrete level score.
    semantic_score = input.is_anomalous ? 0.0f : ScoreMap::LevelToScore(level);

    auto& metrics = ScoringMetrics::Instance();
    metrics.RecordScore(level);
    if (input.is_anomalous) metrics.RecordAnomalous();

    const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start;
    metrics.ObserveAssignDuration(elapsed.count());
}

float SemanticScorer::ComputeFinalScore(float rerank_score,
                                        std::optional<float> enriched_score,
                                        float semantic_score,
                                        bool is_anomalous,
                                        float alpha,
                                        const observability::TraceContext* /*ctx*/) {
    if (alpha < 0.0f || alpha > 1.0f) {
        // §4.4 CX_ERR_SCORING_CONFIG_INVALID — α must be in [0, 1].
        ScoringMetrics::Instance().RecordError(F07ErrorCode::kConfigInvalid);  // §6 error_total
        throw agent_friendly::AgentFriendlyException(MakeScoringError(
            F07ErrorCode::kConfigInvalid,
            {{"alpha_received", alpha},
             {"valid_range", {0.0, 1.0}},
             {"config_source", "config.yaml:scoring.alpha"}},
            "SemanticScorer::ComputeFinalScore: alpha out of [0,1]"));
    }

    // (v1.0.1 M2) anomalous Block explicit priority: even if an enriched_score exists, an anomalous
    // block's effective semantic is the D6 sentinel 0.0 — not coalesce(enriched, 0.0).
    const float effective = is_anomalous
                                ? 0.0f
                                : (enriched_score.has_value() ? *enriched_score : semantic_score);

    const float final_score = rerank_score * (1.0f + alpha * (effective - 0.5f));
    ScoringMetrics::Instance().RecordFinalScore();
    return final_score;
}

}  // namespace cortrix::scoring
