#pragma once
#include <string>
#include <cstdint>

namespace cortrix {

struct InteractionLog {
    std::string id;                    // UUID v4, PRIMARY KEY
    std::string session_id;            // session ID (UUID v4)
    std::string namespace_name;        // tenant/namespace identifier
    std::string user_id;               // user identifier (optional)
    std::string role;                  // "user" | "assistant" | "system"
    std::string content;               // interaction content (query or response)
    std::string query_type;            // "semantic" | "sql" | "hybrid" | "chat"
    std::string status;                // "success" | "error" | "partial"
    int latency_ms = 0;               // response latency (assistant type has value)
    std::string metadata_json;         // extension metadata JSON
    std::string created_at;            // ISO 8601 timestamp
    // MEM04 Memory Immunity (opt-out). Pure ADD — does not touch the frozen columns
    // above. remember=false on a new interaction triggers session-level opt-out (D2).
    bool remember = true;              // whether this interaction may be extracted (D2/D4)
};

}  // namespace cortrix
