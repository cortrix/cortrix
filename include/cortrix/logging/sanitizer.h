#pragma once
// Security hardening — log / structured_data sanitizer.
//
// LogSanitizer scrubs sensitive fields out of any nlohmann::json payload before
// it reaches a log sink or is returned in an error body. Field rules live in
// sensitive_fields.yaml (22 controlled fields, design sec 6.1): P0 secrets are
// redacted, PII is masked, large content is truncated + hashed, correlation
// keys are hashed. The same rule file is shared with the Python SDK so both
// languages sanitize identically (design sec 6.4).
//
// Standalone scope (D-R1): this provides the sanitizer + rule loading + a global
// instance. Wiring it into every existing logger / error call site (enricher, auth,
// import, memory, SDK) is deferred.

#include <string>
#include <unordered_set>

#include <nlohmann/json.hpp>

namespace cortrix::logging {

/// Loads field rules and applies them to JSON payloads. Immutable after
/// construction, so a single instance is safe to share across threads.
class LogSanitizer {
public:
    /// Build from sensitive_fields.yaml text (used by tests and the loader).
    /// Unknown / missing sections default to empty rule sets.
    explicit LogSanitizer(const std::string& yaml_text);

    /// Recursively sanitize a JSON payload: every object key is matched against
    /// the rule sets; nested objects and array elements are walked. Non-matching
    /// scalars pass through untouched. Returns a sanitized copy (input unchanged).
    nlohmann::json Sanitize(const nlohmann::json& payload) const;

    /// Sanitize a single string field by name. Returns the field's sanitized
    /// string form (redacted / masked / truncated / hashed), or the original
    /// value when the field name is not controlled.
    std::string SanitizeField(const std::string& field_name,
                              const std::string& value) const;

    /// True if the field name is in any controlled rule set.
    bool IsControlled(const std::string& field_name) const;

    // ---- Initialize / global accessor (mirrors the design's static API) ----

    /// Load sensitive_fields.yaml from disk and install the process-wide
    /// sanitizer. Call once at startup. On read/parse failure logs a warning and
    /// installs an empty (pass-through) sanitizer so logging never crashes.
    static void Initialize(const std::string& config_path);

    /// Process-wide sanitizer. Returns nullptr if Initialize() was never called;
    /// callers should null-check (a missing sanitizer must not crash logging).
    static const LogSanitizer* Global();

private:
    /// Apply the matching rule to a single JSON value given its key.
    nlohmann::json SanitizeValue(const std::string& key,
                                 const nlohmann::json& value) const;

    std::unordered_set<std::string> p0_redact_;
    std::unordered_set<std::string> p0_mask_;
    std::unordered_set<std::string> p1_pii_;
    std::unordered_set<std::string> p1_truncate_;
    std::unordered_set<std::string> p2_hash_;
};

/// Convenience wrapper for log call sites that hold a single named sensitive
/// value (e.g. an email or api_key) before formatting it into a log message.
/// Routes through the process-wide LogSanitizer so the field is redacted /
/// masked / truncated / hashed per sensitive_fields.yaml.
///
/// Null-safe by design: if LogSanitizer::Initialize() was never called (tests,
/// early startup) the process-wide instance is absent and the value is returned
/// unchanged — logging must never crash because sanitization is unconfigured.
///
/// Why a per-field helper and not a logger/formatter hook: CORTRIX_LOG_* emit
/// free-text messages (fmt::format -> opaque string) with no field keys, so the
/// key-based LogSanitizer cannot reliably scrub an arbitrary formatted line.
/// Sanitization therefore happens at the call site, where the field name is
/// known, by wrapping the sensitive argument in SanitizeForLog("<field>", value).
std::string SanitizeForLog(const std::string& field_name,
                           const std::string& value);

}  // namespace cortrix::logging
