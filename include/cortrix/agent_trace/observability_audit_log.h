#pragma once
#include <cstdint>
#include <string>

namespace cortrix::agent_trace {

/// Forensics log for admin cross-user access. When an
/// admin reads another user's traces/interactions/sources, this layer writes a structured
/// JSON log line (NOT operation_log, NOT a metric — v1.0.5: retention is via
/// log rotation) carrying admin_user_id / target_user_id / endpoint / timestamp so
/// the access is forensically traceable.
///
/// Standalone: BuildLine() is a pure JSON formatter (testable); Emit() writes it to
/// stderr (the OBS_SPEC sink wiring — spdlog/file — is integration, same as
/// ObservabilityContext::LogStructured). The handler calls Emit() only on the
/// admin-cross-user branch.
class ObservabilityAuditLog {
public:
    /// The endpoint label for the forensics line (matches the metric endpoint enum).
    enum class Endpoint { kTraces, kInteractions, kInteractionsSources };

    /// Build the structured JSON log line (no trailing newline). Pure — callers and
    /// tests can inspect the exact payload. `target_user_id` may be empty (the
    /// session/interaction owner was anonymous).
    static std::string BuildLine(const std::string& admin_user_id,
                                 const std::string& target_user_id,
                                 Endpoint endpoint,
                                 int64_t timestamp_ms);

    /// Emit the forensics line for an admin cross-user access (stamps now()). Writes
    /// BuildLine() to stderr in Phase 1.
    static void Emit(const std::string& admin_user_id,
                     const std::string& target_user_id,
                     Endpoint endpoint);

    static const char* ToString(Endpoint endpoint);
};

}  // namespace cortrix::agent_trace
