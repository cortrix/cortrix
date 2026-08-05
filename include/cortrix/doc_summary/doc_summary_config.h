#pragma once
#include <string>

namespace cortrix::doc_summary {

// -----------------------------------------------------------------------------
// Document Summary Index configuration (IGlobalConfig fields).
//
// Read through the generic IGlobalConfig accessors (GetInt/GetFloat/GetBool/
// GetString) under the keys below — this layer does NOT add typed getters to the frozen
// IGlobalConfig scaffolding header (an doc-summary-typed getter would be a D3.5 reverse
// hook, the same approach HyPE took for hype.* and sparse retrieval for retrieval.sparse_top_k).
// The DocSummaryConfig struct + the resolver consume these.
// -----------------------------------------------------------------------------

inline constexpr const char* kMaxCharsKey = "doc_summary.max_chars";
inline constexpr const char* kChunkThresholdKey = "doc_summary.chunk_threshold";
inline constexpr const char* kFallbackScoreThresholdKey =
    "doc_summary.fallback_score_threshold";
inline constexpr const char* kFallbackTopNKey = "doc_summary.fallback_top_n";
inline constexpr const char* kPromptVersionKey = "doc_summary.prompt_version";
inline constexpr const char* kFts5FallbackEnabledKey =
    "doc_summary.fts5_fallback_enabled";

/// §4.4 defaults.
inline constexpr int kMaxCharsDefault = 500;             ///< summary_text upper bound
inline constexpr int kMinChars = 200;                    ///< §1.1 lower bound (200-500)
inline constexpr int kChunkThresholdDefault = 50;        ///< > this → map-reduce
inline constexpr float kFallbackScoreThresholdDefault = 0.7f;  ///< fallback trigger
inline constexpr int kFallbackTopNDefault = 3;           ///< fallback per-doc chunks
inline constexpr const char* kPromptVersionDefault = "v1";
inline constexpr bool kFts5FallbackEnabledDefault = true;  ///< hybrid fallback switch

/// Default LLM model for doc-summary generation (shared with the enricher / HyPE default).
inline constexpr const char* kDefaultLlmModel = "gpt-4o-mini";

/// Map-reduce group size (20 chunks per Map group when chunked).
inline constexpr int kMapGroupSize = 20;

/// Default RRF constant for the doc-discovery hybrid fusion (industry
/// default k=60, same as the sparse path / NS-configurable Phase 2).
inline constexpr int kDocDiscoveryRrfK = 60;

}  // namespace cortrix::doc_summary
