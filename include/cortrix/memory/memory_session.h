#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include "cortrix/memory/interaction_log.h"

namespace cortrix {

struct MemorySession {
    std::string session_id;            // UUID v4
    std::string namespace_name;        // namespace identifier
    std::string user_id;               // user identifier (optional)
    std::string title;                 // session title (optional, auto-generated from first query)
    int interaction_count = 0;         // interaction round count
    std::string doc_id;                // associated Document ID, ULID (source_type=memory_session); empty = none
    std::string created_at;            // ISO 8601
    std::string updated_at;            // ISO 8601
    // Memory immunity (opt-out). Pure ADD — does not touch the frozen columns
    // above. opt_out_at empty = not opted-out; non-empty (ISO 8601) = opted-out time.
    std::string opt_out_at;            // ISO 8601, empty when active (D4)
    std::string opted_out_by;          // "user_manual" | "agent_auto" | "system_auto" | "test" (D4)
};

/// Full session detail (with interaction history)
struct MemorySessionDetail {
    MemorySession session;
    std::vector<InteractionLog> interactions;  // sorted by time ascending
};

}  // namespace cortrix
