#pragma once
#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

#include "cortrix/common/block_types.h"

// Semantic Score — ScoreMap static mapping + ScoringInput.
//
// net-new namespace cortrix::scoring (verified dev=8e189e1 has no such ns/dir, so creating it has no conflict).
namespace cortrix::scoring {

/// Inputs to the Matrix level computation. Carries the processing-depth signals
/// (parser / enricher / image) + the anomaly flag + the block_type (for the
/// Meta-Block special case). All defaults map to Level 0 (a fully-empty input
/// scores the floor, 0.2).
struct ScoringInput {
    std::string parser_name;          ///< "docling" / "paddleocr" / "docling+paddleocr"
    std::string enricher_name;        ///< "null" / "llm" / "contextual" / "hype"
    bool has_image_caption = false;   ///< Vision caption present → image_level 4
    bool is_anomalous = false;        ///< anomalous Block → score=0.0 in the scorer
    uint16_t block_type = 0;          ///< CortrixBlockType (1=FILE … 8=META). META(8)
                                      ///< → ComputeLevel special case returns Level 0 (=0.2).
};

/// Static mapping tool (the sole computation source; the enricher reuses this
/// utility class via LevelToScore). Pure functions, no state.
class ScoreMap {
public:
    /// Level → Score mapping (D3 ruling: strict 5-level discrete). ARCH §5.1.2 global SoT.
    static constexpr float kLevelScore[5] = {0.2f, 0.4f, 0.6f, 0.8f, 1.0f};

    /// D2 ruling — Matrix multi-dimensional decision:
    ///   processing_level = max(parser_level, enricher_level, image_level)
    ///     parser_level:    Docling=2, PaddleOCR=1, Docling+PaddleOCR=2, unknown=0
    ///     enricher_level:  LlmEnricher=4, ContextualRetrieval=3, HyPE=3, NullEnricher/unknown=0
    ///     image_level:     has_image_caption=4, else 0
    /// (v1.0.1 M1 fix) Meta Block special case: input.block_type == kBlockMeta (8) → force returning
    ///   Level 0 (=0.2); aligned with ARCH §5.2.1 metadata-locked semantic_score=0.2, consistent with D7.
    /// Always returns a value in [0, 4] (never out of range — defensive by construction).
    static uint8_t ComputeLevel(const ScoringInput& input);

    /// Level → Score (direct table lookup kLevelScore[level]). Error: level > 4 → CX_ERR_SCORING_LEVEL_INVALID
    /// (throws cortrix::agent_friendly::AgentFriendlyException carrying that code; this
    /// is the §4.4 defensive bottom-out — ComputeLevel never produces an out-of-range
    /// level, so a throw here means a caller passed a raw bad level).
    static float LevelToScore(uint8_t level);

    // --- Matrix sub-level helpers (exposed for targeted UT) ---

    /// parser_name → parser_level (Docling=2, PaddleOCR=1, Docling+PaddleOCR=2, else 0).
    static uint8_t ParserLevel(const std::string& parser_name);
    /// enricher_name → enricher_level (llm=4, contextual=3, hype=3, null/unknown=0).
    static uint8_t EnricherLevel(const std::string& enricher_name);
};

}  // namespace cortrix::scoring
