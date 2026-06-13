#pragma once
#include <cstdint>
#include <string>
#include "nlohmann/json.hpp"

namespace cortrix {

using json = nlohmann::json;

using DocumentId = int64_t;
using BlockId    = uint64_t;  // unified with id/types.h (P-HNSW label); ID-system S3
using TaskId     = int64_t;

}  // namespace cortrix
