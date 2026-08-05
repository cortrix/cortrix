#pragma once
#include "cortrix/tenant/tenant_types.h"

namespace cortrix::tenant {

/// Plan-quota source (dependency inversion). V1.0 OSS injects
/// UnlimitedPlanProvider; Cloud V1 injects StripePlanProvider (Phase 1.5) with no
/// change to QuotaService / the HTTP layer (the DI seam).
class IPlanProvider {
public:
    virtual ~IPlanProvider() = default;

    /// The ceiling for (tenant, quota_type). UnlimitedPlanProvider returns
    /// QuotaLimit::Unlimited() for every input.
    virtual QuotaLimit GetLimit(const TenantId& tenant_id, QuotaType type) = 0;
};

/// V1.0 OSS default -- every quota is unlimited.
class UnlimitedPlanProvider : public IPlanProvider {
public:
    QuotaLimit GetLimit(const TenantId& /*tenant_id*/, QuotaType /*type*/) override {
        return QuotaLimit::Unlimited();
    }
};

}  // namespace cortrix::tenant
