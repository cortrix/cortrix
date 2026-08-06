#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include "cortrix/common/status.h"
#include "cortrix/common/types.h"

namespace cortrix {

struct QueryFilter {
    std::vector<std::string> block_types;   // "FILE", "DATABASE", ... (empty = no filter)
    std::vector<std::string> exclude_types; // block types to exclude (empty = no exclusion)
    std::string source_path;                // glob match, e.g. "/contracts/*" (empty = no filter)
    std::string created_after;              // ISO 8601, e.g. "2025-01-01" (empty = no filter)
    std::string doc_id;                     // pre-filter: fetch all blocks for this doc (empty = disabled)
                                            // bypasses HNSW/BM25; returns chunks in order
};

struct SearchConfig {
    bool enable_vector = true;
    bool enable_bm25   = true;
    bool enable_sql    = true;              // accepted but the SQL route is not executed yet
};

struct QueryRequest {
    std::string query;                      // query text (required)
    std::string namespace_name;             // target namespace (required)
    int top_k = 10;                         // result count (default 10, range 1-100)
    int timeout_ms = 5000;                  // API-level timeout (default 5000ms)
    QueryFilter filters;                    // optional filter conditions
    SearchConfig search_config;             // optional route switches

    // --- Decision-signal request controls (CRAG / routing / granularity) ---
    // All three are additive with safe defaults so an absent param never changes
    // behavior and existing callers (e.g. memory_searcher::ToQueryRequest) are
    // unaffected. They may arrive as query-string params OR JSON body fields; the
    // route handler applies the query-string-wins precedence and validates the
    // enums (route, granularity). FromJson only reads the
    // body form; it does not reject (validation is centralized at the boundary).
    bool explain = false;                   // ?explain — dump QueryContext to meta (default off)
    std::string route = "auto";             // ?route override: auto|simple|complex|chat
    std::string granularity = "auto";       // ?granularity: auto|chunk|doc|both

    /// Parse from JSON body
    /// @return Ok / InvalidArgument
    static Status FromJson(const json& j, QueryRequest* req);

    /// Normalize request parameters (call before Validate).
    /// Applies truncation: if query exceeds kMaxQueryLength chars, truncate and
    /// set was_truncated = true.  Design spec (boundary #13):
    /// "queries > 10000 chars -> truncate to max_query_length (default 2000), warning log"
    /// @return true if query was truncated
    bool Normalize();

    /// Validate parameters
    /// @return Ok / InvalidArgument (with detail message)
    Status Validate() const;

    /// Whether Normalize() truncated the query
    bool was_truncated = false;

    /// Maximum query length after truncation (config default per design = 2000)
    static constexpr size_t kMaxQueryLength = 2000;
};

/// block_type string -> CortrixBlockType enum value
inline int BlockTypeFromString(const std::string& s) {
    if (s == "FILE")     return 1;
    if (s == "SCAN")     return 2;
    if (s == "DATABASE") return 3;
    if (s == "AUDIO")    return 4;
    if (s == "VIDEO")    return 5;
    if (s == "IMAGE")    return 6;
    if (s == "MEMORY")   return 7;
    return -1;
}

/// block_type enum value -> string
inline std::string BlockTypeToString(int type) {
    switch (type) {
        case 1: return "FILE";
        case 2: return "SCAN";
        case 3: return "DATABASE";
        case 4: return "AUDIO";
        case 5: return "VIDEO";
        case 6: return "IMAGE";
        case 7: return "MEMORY";
        default: return "UNKNOWN";
    }
}

}  // namespace cortrix
