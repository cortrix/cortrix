#pragma once
#include <string>

namespace cortrix::spc {

// -----------------------------------------------------------------------------
// HyPE configuration constants (IGlobalConfig fields).
//
// Read through the generic IGlobalConfig accessors (GetInt/GetString) under the
// keys below — HyPE does NOT add typed getters to the frozen IGlobalConfig
// scaffolding header; an F38-typed getter on IGlobalConfig would be a D3.5
// reverse hook (same approach sparse retrieval took for retrieval.sparse_top_k). The S1
// HyPEConfig struct + the S6 NS-config resolver consume these.
// -----------------------------------------------------------------------------

/// IGlobalConfig key: hypothetical questions generated per chunk.
inline constexpr const char* kHypeQuestionsPerChunkKey = "hype.questions_per_chunk";
/// IGlobalConfig key: prompt template version (Phase-2 NS-switchable).
inline constexpr const char* kHypePromptVersionKey = "hype.prompt_version";

/// K = questions per chunk: default 3, NS-configurable 1-10.
inline constexpr int kHypeQuestionsDefault = 3;
inline constexpr int kHypeQuestionsMin = 1;
inline constexpr int kHypeQuestionsMax = 10;

/// Default prompt version. Phase-1 = the single English template v1.
inline constexpr const char* kHypePromptVersionDefault = "v1";

/// Default LLM model for HyPE generation (shared with the enricher default).
inline constexpr const char* kHypeDefaultLlmModel = "gpt-4o-mini";

}  // namespace cortrix::spc
