#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include "cortrix/common/types.h"

namespace cortrix {

struct ResultItem {
    uint64_t     block_id = 0;
    std::string  doc_id;
    float       score = 0.0f;                // RRF fusion score
    std::string source_path;
    std::string block_type;                  // "FILE" / "DATABASE" / ...
    std::string chunk_text;
    json        metadata;                    // block metadata JSON
    int         related_blocks_count = 0;    // multi-route hit count
    uint8_t     hit_routes = 0;              // RouteFlag bitmask
    float       vector_score = 0.0f;         // raw vector similarity score
    float       bm25_score = 0.0f;           // raw BM25 score
};

struct QueryMeta {
    int         total_results = 0;
    int64_t     latency_ms = 0;
    std::vector<std::string> routes_used;    // ["vector", "bm25"]
    bool        degraded = false;
    std::vector<std::string> degraded_routes;
    int         degradation_level = 0;       // 0-3
};

struct SqlResult {
    std::string query;
    std::vector<std::string> columns;
    std::vector<std::vector<json>> rows;
    bool has_result = false;                 // false = sql_result: null
    // metadata-block extensions
    int row_count = 0;
    bool truncated = false;
    std::string error;
    int64_t sql_latency_ms = 0;
};

struct QueryResponse {
    std::vector<ResultItem> results;
    SqlResult sql_result;                    // always null today
    QueryMeta meta;
    bool has_error = false;                  // true when all routes failed (L3)
    std::string error_message;               // user-facing error description

    /// Serialize to JSON
    json ToJson() const;
};

}  // namespace cortrix
