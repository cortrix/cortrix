#include "cortrix/spc_enricher/enricher_startup.h"

namespace cortrix::spc {

const char* ToString(StartupCheck c) {
    switch (c) {
        case StartupCheck::kOk:                  return "ok";
        case StartupCheck::kNullType:            return "null_type";
        case StartupCheck::kApiKeyMissing:       return "api_key_missing";
        case StartupCheck::kEndpointUnreachable: return "endpoint_unreachable";
    }
    return "unknown";
}

StartupCheck StartupValidate(const EnricherConfig& config, llm::IHttpTransport& transport) {
    if (config.type != EnricherType::kLlm) {
        return StartupCheck::kNullType;  // null / local_ner → normal NullEnricher
    }
    if (config.api_key.empty()) {
        return StartupCheck::kApiKeyMissing;  // scenario 2 (§4.4)
    }
    // Endpoint probe: GET {endpoint}/models within startup_probe_timeout_ms (§4.1).
    llm::HttpRequest probe;
    probe.method = llm::HttpMethod::kGet;
    probe.url = config.endpoint + "/models";
    probe.timeout_ms = config.startup_probe_timeout_ms;
    probe.headers["Authorization"] = "Bearer " + config.api_key;
    llm::HttpResponse resp = transport.Send(probe);
    return resp.ok() ? StartupCheck::kOk : StartupCheck::kEndpointUnreachable;  // scenario 3
}

}  // namespace cortrix::spc
