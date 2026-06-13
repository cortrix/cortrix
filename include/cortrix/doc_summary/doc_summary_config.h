#pragma once
#include <string>

namespace cortrix::doc_summary {

// -----------------------------------------------------------------------------
// F41 Document Summary Index configuration (F41 §4.4 IGlobalConfig fields).
//
// Read through the generic IGlobalConfig accessors (GetInt/GetFloat/GetBool/
// GetString) under the keys below — F41 does NOT add typed getters to the frozen
// IGlobalConfig scaffolding header (an F41-typed getter would be a D3.5 reverse
// hook, the same approach F38 took for hype.* and F40 for retrieval.sparse_top_k).
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
inline constexpr int kChunkThresholdDefault = 50;        ///< > this → map-reduce (F41-2)
inline constexpr float kFallbackScoreThresholdDefault = 0.7f;  ///< F41-5 G fallback trigger
inline constexpr int kFallbackTopNDefault = 3;           ///< F41-5 G fallback per-doc chunks
inline constexpr const char* kPromptVersionDefault = "v1";
inline constexpr bool kFts5FallbackEnabledDefault = true;  ///< F41-8 hybrid fallback switch

/// Default LLM model for doc-summary generation (shared with F03 / F38 default).
inline constexpr const char* kDefaultLlmModel = "gpt-4o-mini";

/// Map-reduce group size (F41 §9.2: 20 chunks per Map group when chunked).
inline constexpr int kMapGroupSize = 20;

/// Default RRF constant for the doc-discovery hybrid fusion (F41 §8.2; industry
/// default k=60, same as F40 §9.1 / NS-configurable Phase 2).
inline constexpr int kDocDiscoveryRrfK = 60;

}  // namespace cortrix::doc_summary
