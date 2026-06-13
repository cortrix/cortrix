#pragma once
#include <cstdint>
#include <optional>

#include "cortrix/common/block_header.h"
#include "cortrix/observability/trace_context.h"
#include "cortrix/scoring/score_map.h"

namespace cortrix::scoring {

/// F07 main class (F07 §4.1, D7 ruling — writes both BlockHeader.processing_level + blocks.semantic_score).
///
/// Standalone Block reconciliation (B_R3_BRIEFING §2 / §7 — "run UT against constructed Block/BlockHeader
/// test fixtures"): the F07 §4.1 sketch is `AssignInitialScore(Block& block, ...)` writing
/// `block.header.processing_level` (immutable) + `block.semantic_score` (dynamic). In dev
/// the real header is the frozen 128-byte `cortrix::cortrix_block_header_t` (has
/// processing_level, common/block_header.h:23), and the real `CortrixBlock` (data_types.h)
/// has NO semantic_score member — semantic_score lives in the per-Unit SQLite `blocks`
/// column (added by F07's schema provider) + the block data body (ARCH §5.1.2 / F07 §3).
/// So `AssignInitialScore` here writes the two destinations directly: the real frozen
/// header's `processing_level` byte + an out `float& semantic_score` the caller owns
/// (the SQLite column / data-body field in real wiring). Wiring this into the real
/// SPC pipeline step-7 (S5) + F09 Block Assembly means changing other feature code → D3.5.
class SemanticScorer {
public:
    SemanticScorer() = default;

    /// Called at write time: compute processing_level + initial semantic_score, writing both destinations (D7).
    /// - header.processing_level ← ComputeLevel(input) (immutable, the F09 header byte).
    /// - semantic_score          ← LevelToScore(level), OR 0.0 when input.is_anomalous
    ///                             (D6 sentinel; processing_level is still written with the normal value — anomalous Blocks
    ///                             do not enter P-HNSW, but the header level retains the true processing depth).
    /// `ctx` is the OBS_SPEC §5.3 trace context (nullptr when untraced).
    void AssignInitialScore(cortrix::cortrix_block_header_t& header,
                            float& semantic_score,
                            const ScoringInput& input,
                            const observability::TraceContext* ctx = nullptr);

    /// Query-time fusion (D5 formula C, multiplier mode):
    ///   final_score = rerank_score × (1 + α × (effective_semantic - 0.5))
    ///   effective_semantic = is_anomalous ? 0.0 : coalesce(enriched_score, semantic_score)
    ///     ★ (v1.0.1 M2) anomalous Block explicit priority — forced to 0.0 even when enriched_score exists
    ///       (prevents coalesce(enriched, 0.0) from wrongly overriding the D6 sentinel when F03 enrichment precedes F10 anomaly detection).
    ///   α = 0.2 (Cortrix internal math parameter, v1.0.1: Phase 1 global only, not exposed to NS / Agent).
    ///     effective=0.5 → ×1.0; =1.0 → ×1.1; =0.2 → ×0.94; =0.0 → ×0.9.
    /// Error: α ∉ [0, 1] → CX_ERR_F07_CONFIG_INVALID (throws AgentFriendlyException).
    static float ComputeFinalScore(
        float rerank_score,
        std::optional<float> enriched_score,
        float semantic_score,
        bool is_anomalous = false,
        float alpha = 0.2f,
        const observability::TraceContext* ctx = nullptr);
};

}  // namespace cortrix::scoring
