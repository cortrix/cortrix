#pragma once
#include "cortrix/llm/http_transport.h"
#include "cortrix/spc_enricher.h"

namespace cortrix::spc {

/// Result of the enricher startup validation (design). Drives the
/// three-scenario fallback + the fallback_to_null / endpoint_probe_failed metrics.
enum class StartupCheck {
    kOk,                   ///< type=llm, api_key set, endpoint probe OK → LlmEnricher
    kNullType,             ///< type=null → NullEnricher (normal, no WARN)
    kApiKeyMissing,        ///< type=llm but api_key empty → fallback Null (scenario 2)
    kEndpointUnreachable,  ///< type=llm + api_key set but probe failed → fallback Null (3)
};

const char* ToString(StartupCheck c);

/// Run the startup validation for `config` against `transport` (topic 3.6):
///   type != llm                         → kNullType
///   type == llm, api_key empty          → kApiKeyMissing
///   type == llm, api_key set, probe GET {endpoint}/models within
///       startup_probe_timeout_ms:
///         2xx                           → kOk
///         else (non-2xx / network fail) → kEndpointUnreachable
///
/// 🔌 Standalone: the probe round-trip goes through the injectable transport so
/// it is fully unit-testable with a fake; the REAL network probe at server
/// bootstrap (live endpoint, HTTPS) is integration deferred wiring.
StartupCheck StartupValidate(const EnricherConfig& config, llm::IHttpTransport& transport);

}  // namespace cortrix::spc
