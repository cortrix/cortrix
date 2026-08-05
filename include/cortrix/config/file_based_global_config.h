#pragma once
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "cortrix/common/i_global_config.h"

namespace cortrix::config {

/// File-backed IGlobalConfig (F12 §3.8 FileBasedGlobalConfig — the CE default
/// global-config source, `~/.cortrix/config.json`). Implements the canonical
/// cortrix::IGlobalConfig (D2-pre-9 scaffolding) — does NOT redefine it.
///
/// Loads a flat JSON object of key→value at construction (values coerced to
/// strings); typed getters parse on read (InvalidArgument on malformed, NotFound
/// on missing). Reload() re-reads the file and fires OnChange for changed keys.
///
/// Scope note (S3.1): F12 owns this CE file impl + reuses the dev/test
/// InMemoryGlobalConfig (common/). The other §3.8 sources — PgcortrixGucConfig
/// (PostgreSQL GUC) and WebUIConfig — belong to F14 / P02a respectively
/// and are delivered there on this same canonical interface; F12 (Layer 0) does
/// not stub them (they'd pull cross-Feature deps).
class FileBasedGlobalConfig : public cortrix::IGlobalConfig {
public:
    /// Construct empty (no file). Use Load() / Set() to populate.
    FileBasedGlobalConfig() = default;
    /// Construct and load from `config_path` (missing file → empty config, OK).
    explicit FileBasedGlobalConfig(const std::string& config_path);

    /// (Re)load from `config_path`, replacing current values. Fires OnChange for
    /// each key whose value changed or was added/removed. Returns InvalidArgument
    /// if the file exists but is not a flat JSON object.
    Status Load(const std::string& config_path);
    /// Re-read the path passed to the ctor/last Load (no-op if never loaded).
    Status Reload();

    /// Set/overwrite a value in memory (does not write the file) + fire OnChange.
    void Set(const std::string& key, const std::string& value);

    Result<std::string> GetString(const std::string& key) const override;
    Result<bool>        GetBool(const std::string& key) const override;
    Result<int>         GetInt(const std::string& key) const override;
    Result<float>       GetFloat(const std::string& key) const override;
    void OnChange(std::function<void(const std::string&)> cb) override;

    /// [D3.5 r2 · P4] Persist the agent_llm config in memory through Set() (api_key
    /// encrypted at rest via the shared codec). Does NOT write the file — mirrors
    /// Set()'s in-memory semantics; the CE config-persist path is a Phase-2 concern.
    void SetAgentLlmConfig(const AgentLlmConfig& cfg) override;

private:
    void NotifyLocked(const std::string& key);  // caller holds mu_

    mutable std::mutex mu_;
    std::string path_;
    std::unordered_map<std::string, std::string> values_;
    std::vector<std::function<void(const std::string&)>> callbacks_;
};

}  // namespace cortrix::config
