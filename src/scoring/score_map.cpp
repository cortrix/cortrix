#include "cortrix/scoring/score_map.h"

#include "cortrix/agent_friendly/error.h"
#include "cortrix/scoring/scoring_error.h"
#include "cortrix/scoring/scoring_metrics.h"

namespace cortrix::scoring {

// Note: no out-of-line definition of kLevelScore — a static constexpr member is
// implicitly inline in C++17, and it is only ever odr-used by value here (indexed in
// LevelToScore, read by value in tests), so no separate definition is required.

uint8_t ScoreMap::ParserLevel(const std::string& parser_name) {
    // D2 Matrix: Docling=2, PaddleOCR=1, Docling+PaddleOCR=2 (the structured Docling
    // signal dominates the combined pipeline), anything else (empty/unknown) = 0.
    if (parser_name == "docling" || parser_name == "docling+paddleocr") return 2;
    if (parser_name == "paddleocr") return 1;
    return 0;
}

uint8_t ScoreMap::EnricherLevel(const std::string& enricher_name) {
    // D2 Matrix: LlmEnricher=4, ContextualRetrieval=3, HyPE=3, NullEnricher/unknown=0.
    if (enricher_name == "llm") return 4;
    if (enricher_name == "contextual" || enricher_name == "hype") return 3;
    return 0;
}

uint8_t ScoreMap::ComputeLevel(const ScoringInput& input) {
    // (v1.0.1 M1) Meta Block special case: F08 Meta Block locks semantic_score=0.2 (ARCH §5.2.1)
    // → forced to Level 0, bypassing the Matrix. Checked first so a Meta Block never picks up a
    // parser/enricher/image level.
    if (input.block_type == kBlockMeta) return 0;

    const uint8_t parser_level = ParserLevel(input.parser_name);
    const uint8_t enricher_level = EnricherLevel(input.enricher_name);
    const uint8_t image_level = input.has_image_caption ? 4 : 0;

    uint8_t level = parser_level;
    if (enricher_level > level) level = enricher_level;
    if (image_level > level) level = image_level;
    // Defensive clamp — by construction every sub-level is ≤4, so this is belt-and-braces.
    return level > 4 ? 4 : level;
}

float ScoreMap::LevelToScore(uint8_t level) {
    if (level > 4) {
        // §4.4 defensive bottom-out: ComputeLevel never emits >4, so reaching here means
        // a caller passed a raw bad level. Carry the exact identity + structured_data.
        ScoringMetrics::Instance().RecordError(F07ErrorCode::kLevelInvalid);  // §6 error_total
        throw agent_friendly::AgentFriendlyException(MakeScoringError(
            F07ErrorCode::kLevelInvalid,
            {{"level_received", level}, {"max_allowed", 4}, {"scoring_input", nullptr}},
            "ScoreMap::LevelToScore: level out of range"));
    }
    return kLevelScore[level];
}

}  // namespace cortrix::scoring
