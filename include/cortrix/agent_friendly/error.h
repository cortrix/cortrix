#pragma once
#include <optional>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

namespace cortrix::agent_friendly {

using json = nlohmann::json;

/// GEN-Agent error category (CLAUDE.md 7 principles #4 — enumerable status value). Serializes to
/// the lowercase string the API contract uses.
enum class ErrorCategory {
    kAuth,
    kQuota,
    kTransient,
    kPermanent,
    kTimeout,
};

const char* ToString(ErrorCategory category);

/// Machine-readable, Agent-friendly error (CLAUDE.md GEN-Agent first principle, 4 fields).
/// Shared scaffolding; the structured error body every Feature returns.
struct AgentFriendlyError {
    std::string code;                       ///< CX_ERR_* stable code
    std::string message;                    ///< human-readable detail
    bool retryable = false;                 ///< #6 — machine-readable retry signal
    ErrorCategory category = ErrorCategory::kPermanent;
    std::optional<int> retry_after_ms;      ///< #6 — null when not applicable
    std::optional<json> structured_data;    ///< #5 — structured error payload
};

/// Throwing form for control flow that must unwind. Carries the full error.
class AgentFriendlyException : public std::runtime_error {
public:
    explicit AgentFriendlyException(AgentFriendlyError error);
    const AgentFriendlyError& GetError() const { return error_; }

private:
    AgentFriendlyError error_;
};

/// Serialize to the Agent-friendly contract error body:
/// {code, message, retryable, category, retry_after_ms, structured_data}.
/// retry_after_ms / structured_data become JSON null when unset.
json ToJson(const AgentFriendlyError& error);

}  // namespace cortrix::agent_friendly
