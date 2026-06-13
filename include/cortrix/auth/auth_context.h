#pragma once
#include <string>
#include <vector>

namespace cortrix {

/// Permission bitmask
enum Permission : int {
    kPermRead  = 1,  // 0b001
    kPermWrite = 2,  // 0b010
    kPermAdmin = 4,  // 0b100
};

struct AuthContext {
    std::string tenant_id;
    std::string user_id;
    std::string agent_id;
    std::vector<std::string> namespaces;
    int permissions = 0;

    bool can_read() const { return permissions & kPermRead; }
    bool can_write() const { return permissions & kPermWrite; }
    bool is_admin() const { return permissions & kPermAdmin; }
    bool can_access_namespace(const std::string& ns) const {
        if (namespaces.empty()) return true;
        for (const auto& allowed : namespaces) {
            if (allowed == ns) return true;
        }
        return false;
    }
};

}  // namespace cortrix
