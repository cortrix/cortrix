#pragma once
#include <functional>
#include <string>

#include "cortrix/common/agent_llm_config.h"

namespace cortrix {

class IGlobalConfig;

/// F48 §6.2 storage codec for AgentLlmConfig over the generic IGlobalConfig
/// key/value store. Shared by every IGlobalConfig backend's GetAgentLlmConfig /
/// SetAgentLlmConfig default so the `agent_llm.*` key layout + the api_key
/// encryption-at-rest live in ONE place (not copied per backend).
///
/// Key layout (all under the `agent_llm.` prefix):
///   agent_llm.provider / .model / .base_url / .max_tokens / .temperature
///   agent_llm.api_key_enc  -- AES-256-GCM(api_key), hex(iv||tag||ciphertext)
namespace agent_llm_codec {

/// Read an AgentLlmConfig from `cfg` via GetString (missing keys → struct
/// defaults). api_key is decrypted from `agent_llm.api_key_enc`; a decode failure
/// yields an empty api_key (never throws).
AgentLlmConfig Load(const IGlobalConfig& cfg);

/// Encrypt `in.api_key` and emit the 6 `agent_llm.*` key/value pairs through
/// `set` (the backend's own Set(key,value) — the interface has no generic
/// setter). Caller fires OnChange as appropriate.
void Store(const AgentLlmConfig& in,
           const std::function<void(const std::string&, const std::string&)>& set);

/// Mask an api_key for the read API surface (F48 §6.3 — never return plaintext):
/// "" stays "", otherwise the first 4 + "..." (e.g. "sk-1...").
std::string MaskApiKey(const std::string& api_key);

}  // namespace agent_llm_codec
}  // namespace cortrix
