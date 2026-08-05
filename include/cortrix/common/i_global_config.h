#pragma once
#include <functional>
#include <string>

#include "cortrix/common/agent_llm_config.h"
#include "cortrix/common/result.h"

namespace cortrix {

/// Canonical global configuration interface (scaffolding D2-pre-9; SoT = F12
/// §3.8). The single repo-wide definition — F05/F48/F18a/F13 consume this, they
/// do not redefine it. Production implementations: PgcortrixGucConfig (PostgreSQL
/// GUC) and FileBasedGlobalConfig (~/.cortrix/config.json), delivered later;
/// InMemoryGlobalConfig is provided for dev/tests.
///
/// Sprint 0 baseline = generic key/value accessors + OnChange + POD reserved
/// fields with no external type dependency. Type-specific getters whose return
/// types come from later Waves (F05Config, AgentLlmConfig) are appended to this
/// canonical via each Feature's D3 reverse hook (V14 J3), e.g.:
///   - Wave B F05 → virtual const F05Config& GetF05Config() const = 0;
///   - Wave E F48 → virtual AgentLlmConfig GetAgentLlmConfig() const = 0;
///                  virtual void SetAgentLlmConfig(const AgentLlmConfig&) = 0;
class IGlobalConfig {
public:
    virtual ~IGlobalConfig() = default;

    // ----- Generic key/value accessors -----
    virtual Result<std::string> GetString(const std::string& key) const = 0;
    virtual Result<bool>        GetBool(const std::string& key) const = 0;
    virtual Result<int>         GetInt(const std::string& key) const = 0;
    virtual Result<float>       GetFloat(const std::string& key) const = 0;

    /// Register a callback invoked with the key whenever a value changes.
    virtual void OnChange(std::function<void(const std::string&)> cb) = 0;

    // ----- F48 §6.2 agent_llm config (Wave E reverse hook · D3.5 r2 Wave P) -----
    // NON-pure: the default implementation serializes the 6 AgentLlmConfig fields
    // through the generic key/value store under the `agent_llm.*` namespace, with
    // `api_key` encrypted at rest (AES-256-GCM, process key). Every existing
    // implementation (InMemory / FileBased / PgcortrixGuc) inherits this for free;
    // a backend may override to store the struct natively. SoT = F48 §6.2.

    /// Read the current agent LLM config (api_key decrypted). Missing keys fall back
    /// to the AgentLlmConfig defaults (empty provider => Mock fallback).
    virtual AgentLlmConfig GetAgentLlmConfig() const;

    /// Persist the agent LLM config (api_key encrypted at rest). Admin-only at the
    /// REST layer; this storage call itself does no auth.
    virtual void SetAgentLlmConfig(const AgentLlmConfig& cfg);

    // ----- POD reserved fields -----
    // POD ints, no external dependency, in the Sprint 0 baseline. Consumed
    // directly by the F18a/F13 CleanupScheduler and the MCP idle watcher.
    int operation_log_retention_days   = 30;      ///< F18a topic 5 — CE 30 / Ent 365
    int operation_log_max_rows         = 100000;  ///< F18a topic 5 — CE 100K / Ent 0 (no limit)
    int agent_trace_retention_days     = 90;      ///< F13 topic 3 — same for CE+Ent
    int interaction_log_retention_days = 180;     ///< F13 topic 11 — CE 180 / Ent 365
    int f13_mcp_idle_timeout_seconds   = 1800;    ///< F13 topic 5 — 30 min
};

}  // namespace cortrix
