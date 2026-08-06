#include <cstdint>
#include "cortrix/agent_trace/observability_audit_log.h"

#include <chrono>
#include <cstdio>

#include <nlohmann/json.hpp>

namespace cortrix::agent_trace {

namespace {

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

}  // namespace

const char* ObservabilityAuditLog::ToString(Endpoint endpoint) {
    switch (endpoint) {
        case Endpoint::kTraces:              return "traces";
        case Endpoint::kInteractions:        return "interactions";
        case Endpoint::kInteractionsSources: return "interactions_sources";
    }
    return "unknown";
}

std::string ObservabilityAuditLog::BuildLine(const std::string& admin_user_id,
                                             const std::string& target_user_id,
                                             Endpoint endpoint,
                                             int64_t timestamp_ms) {
    nlohmann::json j;
    j["event"] = "admin_cross_user_access";  // forensics marker
    j["level"] = "warn";
    j["admin_user_id"] = admin_user_id;
    j["target_user_id"] = target_user_id;
    j["endpoint"] = ToString(endpoint);
    j["timestamp_ms"] = timestamp_ms;
    return j.dump();
}

void ObservabilityAuditLog::Emit(const std::string& admin_user_id,
                                 const std::string& target_user_id,
                                 Endpoint endpoint) {
    const std::string line = BuildLine(admin_user_id, target_user_id, endpoint, NowMs());
    std::fprintf(stderr, "%s\n", line.c_str());
}

}  // namespace cortrix::agent_trace
