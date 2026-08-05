#pragma once
// Security hardening — Phase 1 default secret provider.
//
// EnvSecretProvider reads secrets straight from process environment variables.
// This is the Phase 1 default and matches how Postgres / Redis / Weaviate take
// credentials. K8s Secret -> env mounts and docker-compose env_file both funnel
// through here. Cloud providers are added later behind ISecretProvider.

#include <future>
#include <optional>
#include <string>

#include "cortrix/security/i_secret_provider.h"

namespace cortrix::security {

class EnvSecretProvider : public ISecretProvider {
public:
    /// Fetches getenv(key). Returns Ok(nullopt) when the var is unset (the
    /// provider answered; the key simply has no value) — never an error, so
    /// callers can distinguish "unset" from a real provider failure.
    std::future<SecretResult> GetAsync(const std::string& key) override;

    /// Environment is always available.
    bool IsHealthy() const override { return true; }

    std::string provider_type() const override { return "env"; }
};

/// Selects the secret provider from CORTRIX_SECRET_PROVIDER (default "env").
/// Phase 1 only knows "env"; unknown values fall back to env with a warning.
/// Cloud providers ("aws", ...) are registered here in Phase 1.5 / Phase 2.
std::shared_ptr<ISecretProvider> CreateSecretProvider();

}  // namespace cortrix::security
