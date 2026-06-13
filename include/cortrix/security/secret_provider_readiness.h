#pragma once
// F20 Security Hardening — secret_provider readiness component (design sec 8.4).
//
// Bridges the F20-6 ISecretProvider into the F24-owned /ready endpoint: reports
// the secret_provider component as ready iff ISecretProvider::IsHealthy() and
// includes the active provider_type in the /ready body. Registered with the
// ReadinessRegistry; the other 4 readiness components are owned by their
// Features (catalog/F12, vector_index/F01, spc_pipeline/F06, memory_store/MEM).

#include <memory>
#include <string>

#include "cortrix/health/readiness.h"
#include "cortrix/security/i_secret_provider.h"

namespace cortrix::security {

class SecretProviderReadiness : public health::IReadyComponent {
public:
    explicit SecretProviderReadiness(std::shared_ptr<ISecretProvider> provider)
        : provider_(std::move(provider)) {}

    std::string name() const override { return "secret_provider"; }

    health::ComponentReadiness CheckReady() const override {
        health::ComponentReadiness r;
        r.ready = provider_ && provider_->IsHealthy();
        if (provider_) {
            r.detail["provider_type"] = provider_->provider_type();
        }
        return r;
    }

private:
    std::shared_ptr<ISecretProvider> provider_;
};

}  // namespace cortrix::security
