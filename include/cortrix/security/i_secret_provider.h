#pragma once
// F20 Security Hardening — Secret management (design topic 2 / topic 6).
//
// ISecretProvider abstracts where runtime secrets come from. Phase 1 ships only
// EnvSecretProvider (reads process environment variables, the industry default
// used by Postgres / Redis / Weaviate). Cloud providers (AWS Secrets Manager,
// GCP Secret Manager, Azure Key Vault, HashiCorp Vault) are deferred to Phase
// 1.5 / Phase 2 behind this same interface — see PHASE2_BACKLOG
// TD-F20-SECRET-PROVIDERS. The async-first shape mirrors mainstream SaaS SDKs so
// a network-backed provider drops in without changing call sites.

#include <future>
#include <optional>
#include <string>

namespace cortrix::security {

/// Failure modes a secret lookup can report. Phase 1's EnvSecretProvider only
/// ever surfaces kNotFound (env var absent); the remaining values exist so Cloud
/// providers added later need no interface change. These map 1:1 to the
/// CX_ERR_SECRET_* codes in the F20 design (only CX_ERR_SECRET_INVALID_CONFIG
/// and CX_ERR_SECRET_NOT_FOUND are wired in Phase 1).
enum class SecretError {
    kNotFound,             ///< secret key does not exist
    kProviderUnavailable,  ///< backing provider unreachable (Cloud, Phase 2)
    kPermissionDenied,     ///< IAM / ACL denied (Cloud, Phase 2)
    kNetworkTimeout,       ///< provider call timed out (Cloud, Phase 2)
    kInvalidConfig,        ///< provider misconfigured
};

const char* ToString(SecretError error);

/// Result of a secret fetch: either an optional value (ok) or a SecretError.
///
/// A successful lookup carries `std::optional<std::string>`: nullopt means the
/// provider answered but the key is unset (distinct from an error). A failed
/// lookup carries a SecretError. We use a purpose-built result here rather than
/// the repo-wide Result<T> because the failure type is the SecretError enum, not
/// a Status.
class SecretResult {
public:
    static SecretResult Ok(std::optional<std::string> value) {
        SecretResult r;
        r.ok_ = true;
        r.value_ = std::move(value);
        return r;
    }
    static SecretResult Err(SecretError error) {
        SecretResult r;
        r.ok_ = false;
        r.error_ = error;
        return r;
    }

    bool ok() const { return ok_; }
    SecretError error() const { return error_; }
    const std::optional<std::string>& value() const { return value_; }

private:
    bool ok_ = false;
    std::optional<std::string> value_;
    SecretError error_ = SecretError::kNotFound;
};

/// Source of runtime secrets. Implementations must be thread-safe for Get /
/// GetAsync / IsHealthy (cortrix-server reads secrets from request threads).
class ISecretProvider {
public:
    virtual ~ISecretProvider() = default;

    /// Async fetch — the canonical method (mirrors mainstream SaaS SDKs). For a
    /// network-backed Cloud provider this runs the call off-thread; for the
    /// env provider it is a trivially-ready future.
    virtual std::future<SecretResult> GetAsync(const std::string& key) = 0;

    /// Synchronous convenience: blocks on GetAsync and returns the value, or
    /// nullopt on any error (caller that needs the error code uses GetAsync).
    virtual std::optional<std::string> Get(const std::string& key) {
        SecretResult result = GetAsync(key).get();
        return result.ok() ? result.value() : std::nullopt;
    }

    /// Health probe — feeds the F20 /ready endpoint's secret_provider component
    /// (design topic 7). EnvSecretProvider is always healthy.
    virtual bool IsHealthy() const = 0;

    /// Identifies the active provider for /ready diagnostics (e.g. "env").
    virtual std::string provider_type() const = 0;
};

}  // namespace cortrix::security
