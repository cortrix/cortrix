#pragma once
#include <memory>
#include <string>

#include "cortrix/llm/http_transport.h"
#include "cortrix/llm/i_llm_client.h"

namespace cortrix::llm {

/// OpenAI-compatible chat client (scaffolding D2-pre-3 skeleton → F03 Wave B).
///
/// F03 Wave B (S2.1) delivers the real transport: POST {endpoint}/chat/completions
/// with Bearer api_key, JSON body {model, messages:[{role:user, content:prompt}],
/// temperature, max_tokens, response_format}; parses choices[0].message.content +
/// usage.{prompt,completion}_tokens; classifies transport / HTTP / rate-limit
/// failures into Agent-friendly Status categories (CX_ERR_ENRICHER_*).
///
/// 🔌 Network seam: the byte-level round-trip goes through an injectable
/// IHttpTransport (briefing "an injectable transport"). The default ctor uses
/// MakeDefaultHttpTransport() (cpp-httplib); tests inject a fake. Retries +
/// circuit-breaker live in the LlmEnricher layer (F03 §4.2) — this client does a
/// single round-trip per Chat() and reports the outcome; the enricher owns the
/// retry/backoff/breaker policy so all 6+ consumers share one resilience model.
class OpenAiLlmClient : public ILlmClient {
public:
    /// Production ctor — default cpp-httplib transport.
    explicit OpenAiLlmClient(LlmClientConfig config);

    /// Seam ctor — inject a transport (tests / D3.5 alternate transports).
    OpenAiLlmClient(LlmClientConfig config, std::unique_ptr<IHttpTransport> transport);

    ChatCompletionResponse Chat(const std::string& prompt,
                                const LlmCallConfig& config) override;

    const LlmClientConfig& config() const { return config_; }

private:
    LlmClientConfig config_;
    std::unique_ptr<IHttpTransport> transport_;
};

}  // namespace cortrix::llm
