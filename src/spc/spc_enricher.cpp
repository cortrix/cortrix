#include "cortrix/spc_enricher.h"

#include <utility>

#include "cortrix/llm/http_transport.h"
#include "cortrix/llm/openai_client.h"
#include "cortrix/spc_enricher/enricher_error.h"
#include "cortrix/spc_enricher/enricher_metrics.h"
#include "cortrix/spc_enricher/enricher_startup.h"

namespace cortrix::spc {

namespace {

// Build the shared OpenAiLlmClient (topic 4.3) from the resolved EnricherConfig.
std::shared_ptr<llm::ILlmClient> MakeClient(const EnricherConfig& config) {
    llm::LlmClientConfig client_cfg;
    client_cfg.endpoint = config.endpoint;
    client_cfg.api_key = config.api_key;
    client_cfg.default_model = config.model;
    client_cfg.timeout_ms = config.task_timeout_ms;
    client_cfg.circuit_breaker_cooldown_ms = config.circuit_breaker_cooldown_sec * 1000;
    return std::make_shared<llm::OpenAiLlmClient>(std::move(client_cfg));
}

// Map the startup check to the fallback metric + record it, then return the
// NullEnricher (scenarios 2 & 3). The actual LOG_WARN is emitted by the caller
// at server bootstrap (integration wiring); the metric is recorded here so it is
// observable + unit-testable now.
std::unique_ptr<ISpcEnricher> FallbackNull(StartupCheck check) {
    auto& m = EnricherMetrics::Instance();
    if (check == StartupCheck::kApiKeyMissing) {
        m.RecordFallbackToNull(EnricherMetrics::FallbackReason::kApiKeyMissing);  // scenario 2
    } else if (check == StartupCheck::kEndpointUnreachable) {
        m.RecordEndpointProbeFailed();
        m.RecordFallbackToNull(EnricherMetrics::FallbackReason::kEndpointUnreachable);  // scenario 3
    }
    return std::make_unique<NullEnricher>();
}

}  // namespace

// --- EnricherErrorMeta -------------------------------------------------------

EnricherErrorMeta EnricherErrorMeta::FromCode(EnricherErrorCode code, std::string data) {
    const EnricherErrorInfo& info = GetEnricherErrorInfo(code);
    EnricherErrorMeta meta;
    meta.retryable = info.retryable;
    meta.category = info.category;
    // -1 sentinel == N/A (design: "0 / -1 = N/A"); registry null → -1.
    meta.retry_after_ms = info.retry_after_ms.value_or(-1);
    meta.structured_data = std::move(data);
    return meta;
}

// --- EnricherType ------------------------------------------------------------

EnricherType ParseEnricherType(const std::string& s) {
    if (s == "llm") return EnricherType::kLlm;
    if (s == "local_ner") return EnricherType::kLocalNer;
    return EnricherType::kNull;  // default + unknown → fail-soft null path
}

const char* EnricherTypeString(EnricherType t) {
    switch (t) {
        case EnricherType::kNull:     return "null";
        case EnricherType::kLlm:      return "llm";
        case EnricherType::kLocalNer: return "local_ner";
    }
    return "null";  // unreachable for a valid enum
}

// --- NullEnricher ------------------------------------------------------------

EnrichResult NullEnricher::Enrich(const std::string& /*chunk_text*/,
                                  const DocumentMetadata& /*doc_meta*/,
                                  const ChunkContext& /*ctx*/) {
    return EnrichResult{};  // status=0, empty entities/summary, score=0
}

std::vector<EnrichResult> NullEnricher::EnrichBatch(
    const std::vector<ChunkContext>& contexts) {
    return std::vector<EnrichResult>(contexts.size(), EnrichResult{});
}

// --- Factory -----------------------------------------------------------------

std::unique_ptr<ISpcEnricher> CreateEnricher(const EnricherConfig& config,
                                             llm::IHttpTransport& probe_transport) {
    // type != llm and disabled both yield the normal NullEnricher (no WARN).
    if (config.type != EnricherType::kLlm || !config.enabled) {
        return std::make_unique<NullEnricher>();
    }
    // startup validation (topic 3.6): api_key check → endpoint probe.
    StartupCheck check = StartupValidate(config, probe_transport);
    if (check != StartupCheck::kOk) {
        return FallbackNull(check);  // scenario 2 / 3 fallback + metric
    }
    // Probe OK → build the shared OpenAiLlmClient + LlmEnricher (topic 4.3).
    return std::make_unique<LlmEnricher>(config, MakeClient(config));
}

std::unique_ptr<ISpcEnricher> CreateEnricher(const EnricherConfig& config) {
    // Production path: probe via the default cpp-httplib transport. The probe is
    // a real network GET {endpoint}/models at server bootstrap. For the non-LLM /
    // disabled / no-key cases StartupValidate short-circuits before any network
    // use.
    auto transport = llm::MakeDefaultHttpTransport();
    return CreateEnricher(config, *transport);
}

}  // namespace cortrix::spc
