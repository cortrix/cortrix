#include <cstdint>
#include "cortrix/namespace/namespace_manager.h"
#include "cortrix/logging/logging.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <regex>
#include <sys/stat.h>

#include <nlohmann/json.hpp>

namespace cortrix {

using json = nlohmann::json;

namespace {

int64_t NowUnix() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

bool MakeDir(const std::string& path) {
    return mkdir(path.c_str(), 0755) == 0 || errno == EEXIST;
}

}  // namespace

NamespaceManager::NamespaceManager(const std::string& data_dir)
    : data_dir_(data_dir),
      file_path_(data_dir + "/namespaces.json") {}

Status NamespaceManager::Init() {
    MakeDir(data_dir_);

    Status s = LoadFromFile();
    if (!s.ok()) {
        CORTRIX_LOG_WARN("namespace", "Failed to load namespaces.json: {}", s.message());
    }

    // Ensure "default" namespace exists
    bool has_default = false;
    for (const auto& ns : namespaces_) {
        if (ns.name == "default") {
            has_default = true;
            break;
        }
    }
    if (!has_default) {
        NamespaceInfo info;
        info.name = "default";
        info.created_at = NowUnix();
        info.updated_at = info.created_at;
        namespaces_.push_back(info);
        SaveToFile();
    }

    return Status::Ok();
}

Status NamespaceManager::Create(const std::string& name) {
    if (!IsValidName(name)) {
        return Status::InvalidArgument("Namespace name must match [a-zA-Z0-9_-]{1,64}");
    }

    std::lock_guard<std::mutex> lock(mu_);

    for (const auto& ns : namespaces_) {
        if (ns.name == name) {
            return Status::AlreadyExists("Namespace '" + name + "' already exists");
        }
    }

    NamespaceInfo info;
    info.name = name;
    info.created_at = NowUnix();
    info.updated_at = info.created_at;
    namespaces_.push_back(info);

    Status s = SaveToFile();
    if (!s.ok()) {
        namespaces_.pop_back();
        return s;
    }

    return Status::Ok();
}

Status NamespaceManager::Get(const std::string& name, NamespaceInfo* info) {
    std::lock_guard<std::mutex> lock(mu_);

    for (const auto& ns : namespaces_) {
        if (ns.name == name) {
            *info = ns;
            return Status::Ok();
        }
    }

    return Status::NotFound("Namespace '" + name + "' not found");
}

std::vector<NamespaceInfo> NamespaceManager::List(const std::vector<std::string>& allowed) {
    std::lock_guard<std::mutex> lock(mu_);

    std::vector<NamespaceInfo> result;
    for (const auto& ns : namespaces_) {
        if (allowed.empty()) {
            result.push_back(ns);
        } else {
            for (const auto& a : allowed) {
                if (a == ns.name) {
                    result.push_back(ns);
                    break;
                }
            }
        }
    }

    std::sort(result.begin(), result.end(),
              [](const NamespaceInfo& a, const NamespaceInfo& b) {
                  return a.name < b.name;
              });

    return result;
}

Status NamespaceManager::Delete(const std::string& name) {
    if (name == "default") {
        return Status::InvalidArgument("Cannot delete the 'default' namespace");
    }

    std::lock_guard<std::mutex> lock(mu_);

    auto it = std::find_if(namespaces_.begin(), namespaces_.end(),
                           [&name](const NamespaceInfo& ns) { return ns.name == name; });
    if (it == namespaces_.end()) {
        return Status::NotFound("Namespace '" + name + "' not found");
    }

    namespaces_.erase(it);

    Status s = SaveToFile();
    if (!s.ok()) {
        return s;
    }

    return Status::Ok();
}

bool NamespaceManager::IsValidName(const std::string& name) {
    if (name.empty() || name.size() > 64) return false;
    static const std::regex pattern("^[a-zA-Z0-9_-]{1,64}$");
    return std::regex_match(name, pattern);
}

Status NamespaceManager::SaveToFile() {
    json j;
    j["version"] = 1;
    j["namespaces"] = json::array();
    for (const auto& ns : namespaces_) {
        json ns_j;
        ns_j["name"] = ns.name;
        ns_j["created_at"] = ns.created_at;
        ns_j["updated_at"] = ns.updated_at;
        ns_j["doc_count"] = ns.doc_count;
        ns_j["block_count"] = ns.block_count;
        j["namespaces"].push_back(ns_j);
    }

    std::string content = j.dump(2);

    // Atomic write: write tmp -> fsync -> rename backup -> rename
    std::string tmp_path = file_path_ + ".tmp";
    std::string bak_path = file_path_ + ".bak";

    {
        std::ofstream f(tmp_path);
        if (!f.is_open()) {
            return Status::Internal("Failed to write " + tmp_path);
        }
        f << content;
        f.flush();
        f.close();
    }

    // Backup existing file
    std::ifstream existing(file_path_);
    if (existing.good()) {
        existing.close();
        std::rename(file_path_.c_str(), bak_path.c_str());
    }

    // Atomic rename
    if (std::rename(tmp_path.c_str(), file_path_.c_str()) != 0) {
        return Status::Internal("Failed to rename tmp to " + file_path_);
    }

    return Status::Ok();
}

Status NamespaceManager::LoadFromFile() {
    auto tryLoad = [](const std::string& path, std::vector<NamespaceInfo>& result) -> bool {
        std::ifstream f(path);
        if (!f.is_open()) return false;

        try {
            json j = json::parse(f);
            result.clear();
            if (j.contains("namespaces") && j["namespaces"].is_array()) {
                for (const auto& ns_j : j["namespaces"]) {
                    NamespaceInfo info;
                    info.name = ns_j.value("name", "");
                    info.created_at = ns_j.value("created_at", int64_t(0));
                    info.updated_at = ns_j.value("updated_at", int64_t(0));
                    info.doc_count = ns_j.value("doc_count", int64_t(0));
                    info.block_count = ns_j.value("block_count", int64_t(0));
                    result.push_back(info);
                }
            }
            return true;
        } catch (...) {
            return false;
        }
    };

    // Try primary file
    if (tryLoad(file_path_, namespaces_)) {
        return Status::Ok();
    }

    // Try backup
    std::string bak_path = file_path_ + ".bak";
    if (tryLoad(bak_path, namespaces_)) {
        CORTRIX_LOG_WARN("namespace", "Recovered from backup namespaces.json.bak");
        return Status::Ok();
    }

    // Neither exists - start fresh
    namespaces_.clear();
    return Status::Ok();
}

}  // namespace cortrix
