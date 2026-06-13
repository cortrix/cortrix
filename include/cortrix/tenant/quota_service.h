#pragma once
#include <memory>

#include "cortrix/auth/auth_context.h"
#include "cortrix/common/result.h"
#include "cortrix/common/status.h"
#include "cortrix/observability/trace_context.h"
#include "cortrix/tenant/i_plan_provider.h"
#include "cortrix/tenant/tenant_types.h"

typedef struct sqlite3 sqlite3;

namespace cortrix::tenant {

/// Quota service (P09 sec 4.4, topic 4). Reads limits from the injected IPlanProvider;
/// usage tracking (RecordUsage) is a no-op in V1.0 OSS (quota_usage lives in
/// platform.db only from Cloud V1 -- P09 sec 3 / sec 6).
///
/// The optional catalog.db handle lets GetQuota verify the tenant exists; pass
/// nullptr in pure provider-only tests.
class QuotaService {
public:
    explicit QuotaService(std::shared_ptr<IPlanProvider> provider, sqlite3* catalog_db = nullptr)
        : provider_(std::move(provider)), db_(catalog_db) {}

    /// Read the full quota picture for a tenant: usage (all zero in V1.0 OSS) +
    /// per-type limits from the provider. Caller must be a member when a db handle
    /// is available. Errors: CX_ERR_TENANT_NOT_FOUND.
    Result<QuotaInfo> GetQuota(const TenantId& tenant_id, const AuthContext& caller_ctx,
                               const observability::TraceContext* ctx = nullptr);

    /// True iff `usage + amount <= limit` for the (tenant, type) ceiling. V1.0 OSS
    /// (UnlimitedPlanProvider) -> always true.
    bool CheckQuota(const TenantId& tenant_id, QuotaType type, int64_t amount,
                    const observability::TraceContext* ctx = nullptr);

    /// V1.0 OSS: no-op (does NOT write quota_usage; the table is not created).
    /// Cloud V1: persists to platform.db quota_usage.
    void RecordUsage(const TenantId& tenant_id, QuotaType type, int64_t amount,
                     const observability::TraceContext* ctx = nullptr);

    /// Admin override (Cloud V1+). Errors: CX_ERR_TENANT_NOT_FOUND. Writing the
    /// platform.db quota_override row is Cloud V1 -- V1.0 OSS validates + no-ops the
    /// persistence (returns Ok when the tenant exists).
    Status UpdateQuotaOverride(const TenantId& tenant_id, QuotaType type, QuotaLimit new_limit,
                               const AuthContext& admin_ctx,
                               const observability::TraceContext* ctx = nullptr);

private:
    std::shared_ptr<IPlanProvider> provider_;
    sqlite3* db_;  ///< optional borrowed catalog.db handle (tenant existence checks)
};

}  // namespace cortrix::tenant
