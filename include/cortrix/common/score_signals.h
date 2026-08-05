#pragma once

#include <optional>

namespace cortrix {

/// Query-time quality signals produced by ingest-side enrichers/scorers.
///
/// These signals are optional because older Units may not have the enrichment
/// columns, and because not every chunk passes through every enrichment stage.
struct ScoreSignals {
    std::optional<float> enriched_score;  ///< LLM enrichment quality signal
    std::optional<float> semantic_score;  ///< write-time semantic quality signal

    bool HasAny() const {
        return enriched_score.has_value() || semantic_score.has_value();
    }
};

}  // namespace cortrix
