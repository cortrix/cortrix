#pragma once
#include <string>

namespace cortrix::spc {

// -----------------------------------------------------------------------------
// F38 HyPE configuration constants (F38 §4.3 IGlobalConfig fields).
//
// Read through the generic IGlobalConfig accessors (GetInt/GetString) under the
// keys below — F38 does NOT add typed getters to the frozen IGlobalConfig
// scaffolding header; an F38-typed getter on IGlobalConfig would be a D3.5
// reverse hook (same approach F40 took for retrieval.sparse_top_k). The S1
// HyPEConfig struct + the S6 NS-config resolver consume these.
// -----------------------------------------------------------------------------

/// IGlobalConfig key: hypothetical questions generated per chunk (F38-1).
inline constexpr const char* kHypeQuestionsPerChunkKey = "hype.questions_per_chunk";
/// IGlobalConfig key: prompt template version (F38-6, Phase-2 NS-switchable).
inline constexpr const char* kHypePromptVersionKey = "hype.prompt_version";

/// K = questions per chunk: default 3, NS-configurable 1-10 (F38-1 ruling D).
inline constexpr int kHypeQuestionsDefault = 3;
inline constexpr int kHypeQuestionsMin = 1;
inline constexpr int kHypeQuestionsMax = 10;

/// Default prompt version (F38-6). Phase-1 = the single English template v1.
inline constexpr const char* kHypePromptVersionDefault = "v1";

/// Default LLM model for HyPE generation (shared with F03 enricher default).
inline constexpr const char* kHypeDefaultLlmModel = "gpt-4o-mini";

}  // namespace cortrix::spc
