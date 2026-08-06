#pragma once
#include <cstdint>
#include <string>

#include "cortrix/scoring/scoring_error.h"

namespace cortrix::scoring {

/// Scoring observability metrics (subsystem `scoring`). Naming
/// `cortrix_scoring_<metric>_<unit>`. Self-contained dependency-free recorder (same pattern
/// as ImportMetrics / RerankerMetrics); registering into the `/metrics` scrape
/// endpoint is cross-Feature wiring → integration.
///
/// v1.0.2: no ns_id label (on the OBS_SPEC absolute deny list for high-cardinality labels) —
/// per-NS data goes through the OBS_SPEC system stats API
/// (GET /api/v1/system/namespaces/<id>/scoring_stats), not metric labels.
class ScoringMetrics {
public:
    /// Process-wide instance (metrics are global counters).
    static ScoringMetrics& Instance();

    // cortrix_scoring_score_total{level="0|1|2|3|4"} (Counter — semantic_score distribution by level).
    void RecordScore(uint8_t level);
    uint64_t ScoreCount(uint8_t level) const;

    // cortrix_scoring_anomalous_blocks_total (Counter — sentinel 0.0).
    void RecordAnomalous();
    uint64_t AnomalousCount() const;

    // cortrix_scoring_final_score_total (Counter — ComputeFinalScore calls; the reranker reuses this tool).
    void RecordFinalScore();
    uint64_t FinalScoreCount() const;

    // cortrix_scoring_assign_duration_seconds (Histogram — AssignInitialScore call latency).
    void ObserveAssignDuration(double seconds);

    // cortrix_scoring_error_total{code="CX_ERR_SCORING_*"} (Counter — error-code
    // distribution; the 5th locked `scoring` metric). Fed at the two throw sites
    // (ScoreMap::LevelToScore kLevelInvalid / SemanticScorer::ComputeFinalScore kConfigInvalid).
    void RecordError(ScoringErrorCode code);
    uint64_t ErrorCount(ScoringErrorCode code) const;

    /// Render the current values as OpenMetrics text (what the endpoint will serve).
    /// Stable metric names + HELP/TYPE lines.
    std::string Render() const;

    /// Reset all values (test helper — metrics are otherwise process-global).
    void ResetForTest();

private:
    ScoringMetrics() = default;
};

}  // namespace cortrix::scoring
