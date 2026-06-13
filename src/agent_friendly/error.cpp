#include "cortrix/agent_friendly/error.h"

#include <utility>

namespace cortrix::agent_friendly {

const char* ToString(ErrorCategory category) {
    switch (category) {
        case ErrorCategory::kAuth:      return "auth";
        case ErrorCategory::kQuota:     return "quota";
        case ErrorCategory::kTransient: return "transient";
        case ErrorCategory::kPermanent: return "permanent";
        case ErrorCategory::kTimeout:   return "timeout";
    }
    return "permanent";  // unreachable; defensive default
}

AgentFriendlyException::AgentFriendlyException(AgentFriendlyError error)
    : std::runtime_error(error.code + ": " + error.message), error_(std::move(error)) {}

json ToJson(const AgentFriendlyError& error) {
    json j;
    j["code"] = error.code;
    j["message"] = error.message;
    j["retryable"] = error.retryable;
    j["category"] = ToString(error.category);
    j["retry_after_ms"] = error.retry_after_ms.has_value() ? json(*error.retry_after_ms) : json(nullptr);
    j["structured_data"] = error.structured_data.has_value() ? *error.structured_data : json(nullptr);
    return j;
}

}  // namespace cortrix::agent_friendly
