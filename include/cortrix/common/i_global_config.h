#pragma once
#include <functional>
#include <string>

#include "cortrix/common/agent_llm_config.h"
#include "cortrix/common/result.h"

namespace cortrix {

/// Canonical global configuration interface (SoT = the catalog config
/// resolver). The single repo-wide definition — its consumers use it, they
/// do not redefine it. Production implementations: PgcortrixGucConfig (PostgreSQL
/// GUC) and FileBasedGlobalConfig (~/.cortrix/config.json), delivered later;
/// InMemoryGlobalConfig is provided for dev/tests.
///
/// Sprint 0 baseline = generic key/value accessors + OnChange + POD reserved
/// fields with no external type dependency. Type-specific getters whose return
/// types come from later Waves (NamespacePoolConfig, AgentLlmConfig) are appended to this
/// canonical via each Feature's reverse hook (V14 J3), e.g.:
///   - virtual const NamespacePoolConfig& GetNamespacePoolConfig() const = 0;
///   - virtual AgentLlmConfig GetAgentLlmConfig() const = 0;
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

    // ----- agent_llm config -----
    // NON-pure: the default implementation serializes the 6 AgentLlmConfig fields
    // through the generic key/value store under the `agent_llm.*` namespace, with
    // `api_key` encrypted at rest (AES-256-GCM, process key). Every existing
    // implementation (InMemory / FileBased / PgcortrixGuc) inherits this for free;
    // a backend may override to store the struct natively.

    /// Read the current agent LLM config (api_key decrypted). Missing keys fall back
    /// to the AgentLlmConfig defaults (empty provider => Mock fallback).
    virtual AgentLlmConfig GetAgentLlmConfig() const;

    /// Persist the agent LLM config (api_key encrypted at rest). Admin-only at the
    /// REST layer; this storage call itself does no auth.
    virtual void SetAgentLlmConfig(const AgentLlmConfig& cfg);

    // ----- POD reserved fields -----
    // POD ints, no external dependency, in the Sprint 0 baseline. Consumed
    // directly by the CleanupScheduler and the MCP idle watcher.
    int operation_log_retention_days   = 30;      ///< CE 30 / Ent 365
    int operation_log_max_rows         = 100000;  ///< CE 100K / Ent 0 (no limit)
    int agent_trace_retention_days     = 90;      ///< same for CE+Ent
    int interaction_log_retention_days = 180;     ///< CE 180 / Ent 365
    int agent_trace_mcp_idle_timeout_seconds   = 1800;    ///< 30 min
};

}  // namespace cortrix
