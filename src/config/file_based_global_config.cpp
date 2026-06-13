#include "cortrix/config/file_based_global_config.h"

#include <cstdio>
#include <exception>
#include <fstream>
#include <sstream>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include "cortrix/common/agent_llm_config_codec.h"

namespace cortrix::config {

namespace {

using json = nlohmann::json;

// Coerce a JSON scalar to the string form the typed getters parse. Strings pass
// through; bools → "true"/"false"; numbers → their textual form; null → "".
std::string JsonValueToString(const json& v) {
    if (v.is_string())  return v.get<std::string>();
    if (v.is_boolean()) return v.get<bool>() ? "true" : "false";
    if (v.is_number_integer())  return std::to_string(v.get<long long>());
    if (v.is_number_unsigned()) return std::to_string(v.get<unsigned long long>());
    if (v.is_number_float()) {
        std::ostringstream os;
        os << v.get<double>();
        return os.str();
    }
    if (v.is_null()) return "";
    // objects/arrays: store the compact JSON text (callers that need structure
    // read the raw string; flat config keys are expected to be scalars).
    return v.dump();
}

}  // namespace

FileBasedGlobalConfig::FileBasedGlobalConfig(const std::string& config_path) {
    // Best-effort load at construction; a missing file is not an error (empty
    // config). A malformed file leaves values_ empty (Load's Status is dropped
    // here — callers wanting to detect that should call Load() explicitly).
    Load(config_path);
}

Status FileBasedGlobalConfig::Load(const std::string& config_path) {
    std::unordered_map<std::string, std::string> parsed;

    std::ifstream f(config_path);
    if (f.is_open()) {
        json doc;
        try {
            f >> doc;
        } catch (const std::exception& e) {
            return Status::InvalidArgument(
                std::string("config file is not valid JSON: ") + e.what());
        }
        if (!doc.is_object()) {
            return Status::InvalidArgument(
                "config file must be a flat JSON object: " + config_path);
        }
        for (auto it = doc.begin(); it != doc.end(); ++it) {
            parsed[it.key()] = JsonValueToString(it.value());
        }
    }
    // A missing file → empty config (valid); path is still remembered for Reload.

    std::lock_guard<std::mutex> lock(mu_);
    path_ = config_path;
    // Fire OnChange for every key whose value changed or was added/removed.
    for (const auto& [k, v] : parsed) {
        auto old = values_.find(k);
        if (old == values_.end() || old->second != v) NotifyLocked(k);
    }
    for (const auto& [k, v] : values_) {
        if (parsed.find(k) == parsed.end()) NotifyLocked(k);  // removed
    }
    values_ = std::move(parsed);
    return Status::Ok();
}

Status FileBasedGlobalConfig::Reload() {
    std::string p;
    {
        std::lock_guard<std::mutex> lock(mu_);
        p = path_;
    }
    if (p.empty()) return Status::Ok();  // never loaded → nothing to do
    return Load(p);
}

void FileBasedGlobalConfig::Set(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = values_.find(key);
    const bool changed = (it == values_.end() || it->second != value);
    values_[key] = value;
    if (changed) NotifyLocked(key);
}

void FileBasedGlobalConfig::NotifyLocked(const std::string& key) {
    for (auto& cb : callbacks_) cb(key);
}

Result<std::string> FileBasedGlobalConfig::GetString(const std::string& key) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = values_.find(key);
    if (it == values_.end()) {
        return Status::NotFound("config key not found: " + key);
    }
    return it->second;
}

Result<bool> FileBasedGlobalConfig::GetBool(const std::string& key) const {
    Result<std::string> raw = GetString(key);
    if (!raw.ok()) return raw.status();
    const std::string& v = raw.value();
    if (v == "true" || v == "1") return true;
    if (v == "false" || v == "0") return false;
    return Status::InvalidArgument("config key '" + key + "' is not a bool: " + v);
}

Result<int> FileBasedGlobalConfig::GetInt(const std::string& key) const {
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

Result<float> FileBasedGlobalConfig::GetFloat(const std::string& key) const {
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

void FileBasedGlobalConfig::OnChange(std::function<void(const std::string&)> cb) {
    std::lock_guard<std::mutex> lock(mu_);
    callbacks_.push_back(std::move(cb));
}

void FileBasedGlobalConfig::SetAgentLlmConfig(const AgentLlmConfig& cfg) {
    agent_llm_codec::Store(cfg, [this](const std::string& k, const std::string& v) {
        Set(k, v);
    });
}

}  // namespace cortrix::config
