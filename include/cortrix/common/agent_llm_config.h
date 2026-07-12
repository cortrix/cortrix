#pragma once
#include <string>

namespace cortrix {

/// F48 §6.2 agent LLM configuration. A plain POD (no external deps) so it can live
/// on the canonical IGlobalConfig getter/setter without pulling F48 into the
/// scaffolding. `api_key` is stored encrypted at rest (the setter encrypts; the
/// getter decrypts) and masked on the read API surface (F48 §6.3). Defaults match
/// the design (max_tokens=4096 / temperature=0.7); an empty `provider` means
/// "unconfigured → Mock provider fallback" (F48 §7.3).
struct AgentLlmConfig {
    std::string provider;        ///< openai | claude | ollama | glm | deepseek | mock
    std::string api_key;         ///< encrypted at rest (F48 §6.2)
    std::string model;
    std::string base_url;
    int max_tokens = 4096;
    double temperature = 0.7;
    // V1.5 reserved: tenant_id (per-tenant config).
};

/// Ceiling for `max_tokens` accepted through the PUT route (QA 2026-07-12
/// FYI-4). Mirrors the enricher-side cap: 32k is the realistic upper budget of
/// the supported providers; anything above is a runaway request, clamped so
/// the persisted config (and its echoed read shape) stays sane.
inline constexpr int kAgentLlmMaxTokensCap = 32768;

}  // namespace cortrix
