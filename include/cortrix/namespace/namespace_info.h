#pragma once
#include <string>
#include <cstdint>

namespace cortrix {

struct NamespaceInfo {
    std::string name;
    int64_t created_at = 0;
    int64_t updated_at = 0;
    int64_t doc_count = 0;
    int64_t block_count = 0;
};

}  // namespace cortrix
