#pragma once
#include <string>

namespace cortrix::spc {

// -----------------------------------------------------------------------------
// F35 Contextual Retrieval configuration constants (F35 §6.2 IGlobalConfig
// fields).
//
// Read through the generic IGlobalConfig accessors (GetString/GetInt/GetBool)
// under the keys below — F35 does NOT add typed getters to the frozen
// IGlobalConfig scaffolding header (an F35-typed getter would be a D3.5 reverse
// hook, same approach F38 took for hype.* and F40 for retrieval.sparse_top_k).
// The ContextualRetrievalConfig struct + the NS-config three-layer resolver
// (§6.2) consume these.
// -----------------------------------------------------------------------------

/// IGlobalConfig key: master enable for the contextual-retrieval enricher (§6.2).
inline constexpr const char* kContextualEnabledKey = "contextual_retrieval.enabled";
/// IGlobalConfig key: prompt template override (§6.2 — empty == built-in default).
inline constexpr const char* kContextualPromptTemplateKey =
    "contextual_retrieval.prompt_template";
/// IGlobalConfig key: LLM max output tokens for the prefix (§6.2, default 80).
inline constexpr const char* kContextualMaxOutputTokensKey =
    "contextual_retrieval.max_output_tokens";
/// IGlobalConfig key: LLM model for prefix generation (§6.2).
inline constexpr const char* kContextualLlmModelKey =
    "contextual_retrieval.llm_model";
/// IGlobalConfig key: per-call LLM timeout in ms for prefix generation. The
/// §6.1 10s default assumed sub-10s provider latency; large prompts on a
/// loaded provider (observed live 2026-07-08: GLM evening peak) exceed it on
/// every attempt, so the affected chunks can never be enriched — not by the
/// inline call, not by the retry sweeper. Absent/unparseable keeps the 10s
/// default (fail-soft), so existing deployments are unchanged.
inline constexpr const char* kContextualTimeoutMsKey =
    "contextual_retrieval.timeout_ms";
/// Floor for the configurable timeout (guards nonsense values like 0/negative).
inline constexpr int kContextualTimeoutMsMin = 1000;

/// Default prefix length in tokens (F35-3 ruling B+D: 80 default, NS-configurable
/// 40-200).
inline constexpr int kContextualMaxOutputTokensDefault = 80;
inline constexpr int kContextualMaxOutputTokensMin = 40;
inline constexpr int kContextualMaxOutputTokensMax = 200;

/// Default LLM model for contextual prefix generation (shared with the F03
/// enricher / F38 HyPE default).
inline constexpr const char* kContextualDefaultLlmModel = "gpt-4o-mini";

/// LLM call timeout for prefix generation (F35 §6.1 = 10s).
inline constexpr int kContextualTimeoutMs = 10000;

/// L3 transient-retry policy (F35 §7.3): N=3 attempts, exponential backoff
/// 1s/2s/4s. The backoff base is exposed so tests can assert the schedule without
/// sleeping (the enricher returns the planned delay; real sleeping is the caller's
/// resilience layer at D3.5).
inline constexpr int kContextualMaxAttempts = 3;
inline constexpr int kContextualBackoffBaseMs = 1000;

}  // namespace cortrix::spc
