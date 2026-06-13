#pragma once
#include <string>
#include <vector>
#include <mutex>

#include "cortrix/common/status.h"
#include "cortrix/namespace/namespace_info.h"

namespace cortrix {

class NamespaceManager {
public:
    explicit NamespaceManager(const std::string& data_dir);

    /// Initialize: load namespaces.json, create "default" if missing
    Status Init();

    /// Create new namespace
    /// @return Ok / InvalidArgument / AlreadyExists
    Status Create(const std::string& name);

    /// Get single namespace info
    /// @return Ok / NotFound
    Status Get(const std::string& name, NamespaceInfo* info);

    /// List all namespaces (filtered by allowed list)
    std::vector<NamespaceInfo> List(const std::vector<std::string>& allowed = {});

    /// Delete namespace
    /// @return Ok / NotFound / InvalidArgument("default" cannot be deleted)
    Status Delete(const std::string& name);

private:
    static bool IsValidName(const std::string& name);
    Status SaveToFile();
    Status LoadFromFile();

    std::string data_dir_;
    std::string file_path_;
    mutable std::mutex mu_;
    std::vector<NamespaceInfo> namespaces_;
};

}  // namespace cortrix
