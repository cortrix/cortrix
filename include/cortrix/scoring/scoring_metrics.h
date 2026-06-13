#pragma once
#include <cstdint>
#include <string>

#include "cortrix/scoring/scoring_error.h"

namespace cortrix::scoring {

/// F07 observability metrics (F07 §6, OBSERVABILITY_SPEC subsystem `scoring`). Naming
/// `cortrix_f07_<metric>_<unit>`. Self-contained dependency-free recorder (same pattern
/// as ImportMetrics / RerankerMetrics); registering into the F24 `/metrics` scrape
/// endpoint is cross-Feature wiring → D3.5.
///
/// v1.0.2 (F07 §6 C1): no ns_id label (on the OBS_SPEC §3.2 absolute deny list for high-cardinality labels) —
/// per-NS data goes through the OBS_SPEC §3.4 system stats API
/// (GET /api/v1/system/namespaces/<id>/f07_stats), not metric labels.
class ScoringMetrics {
public:
    /// Process-wide instance (metrics are global counters).
    static ScoringMetrics& Instance();

    // cortrix_f07_score_total{level="0|1|2|3|4"} (Counter — semantic_score distribution by level).
    void RecordScore(uint8_t level);
    uint64_t ScoreCount(uint8_t level) const;

    // cortrix_f07_anomalous_blocks_total (Counter — D6 sentinel 0.0).
    void RecordAnomalous();
    uint64_t AnomalousCount() const;

    // cortrix_f07_final_score_total (Counter — ComputeFinalScore calls, F02 side reuses the F07 tool).
    void RecordFinalScore();
    uint64_t FinalScoreCount() const;

    // cortrix_f07_assign_duration_seconds (Histogram — AssignInitialScore call latency).
    void ObserveAssignDuration(double seconds);

    // cortrix_f07_error_total{code="CX_ERR_F07_*"} (Counter — F07 §6 / §4.4 error-code
    // distribution; the 5th locked `scoring` metric). Fed at the two F07 throw sites
    // (ScoreMap::LevelToScore kLevelInvalid / SemanticScorer::ComputeFinalScore kConfigInvalid).
    void RecordError(F07ErrorCode code);
    uint64_t ErrorCount(F07ErrorCode code) const;

    /// Render the current values as OpenMetrics text (what the F24 endpoint will serve).
    /// Stable metric names + HELP/TYPE lines.
    std::string Render() const;

    /// Reset all values (test helper — metrics are otherwise process-global).
    void ResetForTest();

private:
    ScoringMetrics() = default;
};

}  // namespace cortrix::scoring
