#pragma once
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "cortrix/common/i_global_config.h"

namespace cortrix {

/// Map-backed IGlobalConfig for dev and tests (scaffolding). Production
/// config sources (pgcortrix GUC / config.json) implement the same interface.
/// Set() updates a value and fires OnChange callbacks; typed getters parse the
/// stored string, returning InvalidArgument on a malformed value and NotFound on
/// a missing key.
class InMemoryGlobalConfig : public IGlobalConfig {
public:
    /// Set/overwrite a value (stored as string) and notify OnChange callbacks.
    void Set(const std::string& key, const std::string& value);

    Result<std::string> GetString(const std::string& key) const override;
    Result<bool>        GetBool(const std::string& key) const override;
    Result<int>         GetInt(const std::string& key) const override;
    Result<float>       GetFloat(const std::string& key) const override;

    void OnChange(std::function<void(const std::string&)> cb) override;

    /// [integration r2 · P4] Persist the agent_llm config through Set() (api_key encrypted
    /// at rest via the shared codec). Fires OnChange per touched key.
    void SetAgentLlmConfig(const AgentLlmConfig& cfg) override;

private:
    std::unordered_map<std::string, std::string> values_;
    std::vector<std::function<void(const std::string&)>> callbacks_;
};

}  // namespace cortrix
