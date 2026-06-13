#include "cortrix/common/in_memory_global_config.h"

#include <stdexcept>

#include "cortrix/common/agent_llm_config_codec.h"

namespace cortrix {

void InMemoryGlobalConfig::Set(const std::string& key, const std::string& value) {
    values_[key] = value;
    for (auto& cb : callbacks_) {
        cb(key);
    }
}

Result<std::string> InMemoryGlobalConfig::GetString(const std::string& key) const {
    auto it = values_.find(key);
    if (it == values_.end()) {
        return Status::NotFound("config key not found: " + key);
    }
    return it->second;
}

Result<bool> InMemoryGlobalConfig::GetBool(const std::string& key) const {
    Result<std::string> raw = GetString(key);
    if (!raw.ok()) return raw.status();
    const std::string& v = raw.value();
    if (v == "true" || v == "1") return true;
    if (v == "false" || v == "0") return false;
    return Status::InvalidArgument("config key '" + key + "' is not a bool: " + v);
}

Result<int> InMemoryGlobalConfig::GetInt(const std::string& key) const {
    Result<std::string> raw = GetString(key);
    if (!raw.ok()) return raw.status();
    try {
        std::size_t consumed = 0;
        int parsed = std::stoi(raw.value(), &consumed);
        if (consumed != raw.value().size()) {
            return Status::InvalidArgument("config key '" + key + "' is not an int: " + raw.value());
        }
        return parsed;
    } catch (const std::exception&) {
        return Status::InvalidArgument("config key '" + key + "' is not an int: " + raw.value());
    }
}

Result<float> InMemoryGlobalConfig::GetFloat(const std::string& key) const {
    Result<std::string> raw = GetString(key);
    if (!raw.ok()) return raw.status();
    try {
        std::size_t consumed = 0;
        float parsed = std::stof(raw.value(), &consumed);
        if (consumed != raw.value().size()) {
            return Status::InvalidArgument("config key '" + key + "' is not a float: " + raw.value());
        }
        return parsed;
    } catch (const std::exception&) {
        return Status::InvalidArgument("config key '" + key + "' is not a float: " + raw.value());
    }
}

void InMemoryGlobalConfig::OnChange(std::function<void(const std::string&)> cb) {
    callbacks_.push_back(std::move(cb));
}

void InMemoryGlobalConfig::SetAgentLlmConfig(const AgentLlmConfig& cfg) {
    agent_llm_codec::Store(cfg, [this](const std::string& k, const std::string& v) {
        Set(k, v);
    });
}

}  // namespace cortrix
