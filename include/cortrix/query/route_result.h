#pragma once
#include <vector>
#include <string>
#include <cstdint>

namespace cortrix {

enum class RouteStatus {
    kSuccess = 0,
    kTimeout = 1,
    kError   = 2,
    kSkipped = 3,       // disabled by search_config or degradation
};

struct RouteResultItem {
    uint64_t block_id = 0;
    std::string doc_id;
    int     chunk_index = 0;
    float   raw_score = 0.0f;       // original score (vector: distance, BM25: rank score)
};

struct RouteResult {
    std::string route_name;                  // "vector" / "bm25" / "sql"
    RouteStatus status = RouteStatus::kSkipped;
    std::string error_message;               // error/timeout message
    int64_t latency_us = 0;                  // route execution time (microseconds)
    std::vector<RouteResultItem> items;      // sorted by raw_score

    bool ok() const { return status == RouteStatus::kSuccess; }
    bool has_results() const { return ok() && !items.empty(); }
};

}  // namespace cortrix
