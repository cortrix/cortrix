#pragma once

namespace cortrix::llm {

/// Neutral failure tokens the (feature-agnostic) OpenAiLlmClient prefixes onto
/// ChatCompletionResponse.status.message(). The client must not know about any
/// one consumer's domain codes (CX_ERR_ENRICHER_* etc.), so it speaks these
/// generic tokens; each consumer maps them to its own error model. F03's
/// LlmEnricher (§4.2) maps:
///   CX_LLM_TRANSPORT / CX_LLM_HTTP(5xx) → CX_ERR_ENRICHER_LLM_API
///   CX_LLM_RATE_LIMIT                    → CX_ERR_ENRICHER_RATE_LIMIT
///   CX_LLM_HTTP(4xx) / CX_LLM_BAD_BODY   → CX_ERR_ENRICHER_LLM_API (transient) /
///                                          treated per §5.1
/// (timeout is detected by the enricher's own ThreadPool deadline, not here).
namespace llm_tokens {
inline constexpr char kTransport[] = "CX_LLM_TRANSPORT";   ///< no HTTP status reached
inline constexpr char kHttp[] = "CX_LLM_HTTP";             ///< got an HTTP status (4xx/5xx)
inline constexpr char kRateLimit[] = "CX_LLM_RATE_LIMIT";  ///< HTTP 429
inline constexpr char kBadBody[] = "CX_LLM_BAD_BODY";      ///< 2xx but unparseable
}  // namespace llm_tokens

}  // namespace cortrix::llm
