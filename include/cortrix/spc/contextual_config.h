#pragma once
#include <string>

namespace cortrix::spc {

// -----------------------------------------------------------------------------
// Contextual Retrieval configuration constants (IGlobalConfig
// fields).
//
// Read through the generic IGlobalConfig accessors (GetString/GetInt/GetBool)
// under the keys below — this stage does NOT add typed getters to the frozen
// IGlobalConfig scaffolding header (an contextual-typed getter would be a D3.5 reverse
// hook, same approach HyPE took for hype.* and sparse retrieval for retrieval.sparse_top_k).
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

/// Default prefix length in tokens (80 default, NS-configurable
/// 40-200).
inline constexpr int kContextualMaxOutputTokensDefault = 80;
inline constexpr int kContextualMaxOutputTokensMin = 40;
inline constexpr int kContextualMaxOutputTokensMax = 200;

/// IGlobalConfig key: §8 injection-guard chars-per-token multiplier. The guard
/// rejects a generation longer than max_output_tokens x this value, COUNTING
/// BYTES (no tokenizer in this seam). The historical hardcoded 2 under-counts
/// real text — 80 English tokens is ~320+ chars, CJK is 3 bytes/char in UTF-8 —
/// so legitimate contexts were rejected as injection and the contextualized
/// vector silently dropped (deep-QA finding 2026-07-10; D12 live 5k evidence
/// 2026-07-11: DeepSeek's natural ~400-byte outputs hit a 41% false-reject
/// rate at 2, and 4 would still under-count (4x80=320 < 400)). Default raised
/// to 6 (6x80=480 bytes, field-validated: 99.55% backfill repair rate);
/// deployments can still tighten or widen via the config key.
inline constexpr const char* kContextualGuardCharsPerTokenKey =
    "contextual_retrieval.guard_chars_per_token";
inline constexpr int kContextualGuardCharsPerTokenDefault = 6;
inline constexpr int kContextualGuardCharsPerTokenMin = 1;
inline constexpr int kContextualGuardCharsPerTokenMax = 20;

/// Default LLM model for contextual prefix generation (shared with the
/// enricher / HyPE default).
inline constexpr const char* kContextualDefaultLlmModel = "gpt-4o-mini";

/// LLM call timeout for prefix generation (10s).
inline constexpr int kContextualTimeoutMs = 10000;

/// L3 transient-retry policy: N=3 attempts, exponential backoff
/// 1s/2s/4s. The backoff base is exposed so tests can assert the schedule without
/// sleeping (the enricher returns the planned delay; real sleeping is the caller's
/// resilience layer at D3.5).
inline constexpr int kContextualMaxAttempts = 3;
inline constexpr int kContextualBackoffBaseMs = 1000;

}  // namespace cortrix::spc
