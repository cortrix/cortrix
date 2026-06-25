#pragma once
#include <string>

#include "cortrix/common/status.h"

namespace cortrix::llm {

/// Per-call generation parameters; empty/zero fields fall back to the client's
/// LlmClientConfig defaults.
struct LlmCallConfig {
    std::string model;             ///< override default model; empty = client default
    double temperature = 0.0;
    int max_tokens = 1024;
    int timeout_ms = 0;            ///< 0 = use client default
    std::string response_format;   ///< e.g. "json_object" for structured output; empty = text
};

/// Client-level configuration: endpoint, auth, defaults, resilience. Sourced
/// from IGlobalConfig GUCs (enricher.endpoint / enricher.api_key, F03 topic 2.1).
struct LlmClientConfig {
    std::string endpoint;                  ///< OpenAI-compatible base URL
    std::string api_key;
    std::string default_model = "gpt-4o-mini";
    int timeout_ms = 30000;
    int max_retries = 2;
    int circuit_breaker_cooldown_ms = 60000;  ///< F03 topic 3 (cooldown=60s)
};

/// Result of a Chat() call. Chat returns this by value (not Result<T>) so the
/// per-call outcome — including token usage for budget caps — travels with the
/// response. `status.ok()` indicates success; on failure `content` is empty.
struct ChatCompletionResponse {
    Status status;                 ///< ok() == success; otherwise the error
    std::string content;           ///< assistant message text
    std::string content_source = "message.content";  ///< response field used as content
    std::string model;             ///< model that served the request
    std::string finish_reason;     ///< "stop" / "length" / ...
    int content_length = 0;
    int reasoning_content_length = 0;
    int prompt_tokens = 0;
    int completion_tokens = 0;

    bool ok() const { return status.ok(); }
};

/// Shared OpenAI-compatible chat LLM client interface (scaffolding D2-pre-3).
/// Consumers (roster, F03 §2.3): F03 LlmEnricher + F35 / F38 / F41 / F15 /
/// MEM02 / F36 — 7 features total. Mock via tests/mocks/mock_llm_client.h.
class ILlmClient {
public:
    virtual ~ILlmClient() = default;

    /// Send a single-prompt chat completion. Never throws; failures (network,
    /// auth, rate limit, circuit open) are reported in the response status with
    /// Agent-friendly categories.
    virtual ChatCompletionResponse Chat(const std::string& prompt,
                                        const LlmCallConfig& config) = 0;
};

}  // namespace cortrix::llm
