#include "cortrix/security/env_secret_provider.h"

#include <cstdlib>
#include <memory>

#include "cortrix/logging/logging.h"

namespace cortrix::security {

const char* ToString(SecretError error) {
    switch (error) {
        case SecretError::kNotFound:            return "not_found";
        case SecretError::kProviderUnavailable: return "provider_unavailable";
        case SecretError::kPermissionDenied:    return "permission_denied";
        case SecretError::kNetworkTimeout:      return "network_timeout";
        case SecretError::kInvalidConfig:       return "invalid_config";
    }
    return "not_found";  // unreachable; defensive default
}

std::future<SecretResult> EnvSecretProvider::GetAsync(const std::string& key) {
    // Deferred launch: env reads are cheap and synchronous, so there is no
    // benefit to spawning a thread. The future is resolved when .get() is
    // called, keeping the async API contract without thread overhead.
    return std::async(std::launch::deferred, [key]() {
        const char* val = std::getenv(key.c_str());
        // Var absent -> Ok(nullopt), NOT an error: the provider answered, the
        // key just has no value. Cloud providers reserve kNotFound for the
        // distinct "key does not exist in the vault" case.
        if (val == nullptr) {
            return SecretResult::Ok(std::nullopt);
        }
        return SecretResult::Ok(std::optional<std::string>(val));
    });
}

std::shared_ptr<ISecretProvider> CreateSecretProvider() {
    const char* provider_type = std::getenv("CORTRIX_SECRET_PROVIDER");
    std::string type = provider_type ? std::string(provider_type) : "env";

    if (type == "env") {
        return std::make_shared<EnvSecretProvider>();
    }

    // Phase 1.5 / Phase 2: "aws" / "gcp" / "azure" / "vault" providers are
    // registered here behind ISecretProvider. Until then, an unknown value is
    // not fatal — fall back to env so a misconfigured deploy still starts.
    CORTRIX_LOG_WARN("security",
                     "Unknown CORTRIX_SECRET_PROVIDER='{}', falling back to env provider",
                     type);
    return std::make_shared<EnvSecretProvider>();
}

}  // namespace cortrix::security
