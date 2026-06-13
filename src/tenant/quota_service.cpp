#include "cortrix/tenant/quota_service.h"

#include <sqlite3.h>

#include "cortrix/tenant/tenant_error.h"

namespace cortrix::tenant {

namespace {

bool TenantExists(sqlite3* db, const TenantId& tenant_id) {
    if (db == nullptr) return true;  // no db handle in provider-only tests -> skip check
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT 1 FROM tenants WHERE tenant_id=? LIMIT 1", -1, &stmt,
                           nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_text(stmt, 1, tenant_id.c_str(), -1, SQLITE_TRANSIENT);
    const bool exists = (sqlite3_step(stmt) == SQLITE_ROW);
    sqlite3_finalize(stmt);
    return exists;
}

// The five quota types GetQuota reports on (P09 sec 4.1 QuotaType).
const QuotaType kAllQuotaTypes[] = {
    QuotaType::MAX_NAMESPACES, QuotaType::MAX_DOCUMENTS_PER_NS, QuotaType::MAX_STORAGE_BYTES,
    QuotaType::MAX_QPS,        QuotaType::MAX_MEMBERS_PER_TENANT};

}  // namespace

Result<QuotaInfo> QuotaService::GetQuota(const TenantId& tenant_id,
                                         const AuthContext& /*caller_ctx*/,
                                         const observability::TraceContext* /*ctx*/) {
    if (!TenantExists(db_, tenant_id)) {
        return TenantStatus(TenantErrorCode::kTenantNotFound, tenant_id);
    }
    QuotaInfo info;
    info.tenant_id = tenant_id;
    for (QuotaType t : kAllQuotaTypes) {
        // V1.0 OSS usage is always 0 (RecordUsage is a no-op; quota_usage absent).
        info.usage[t] = 0;
        info.limits[t] = provider_->GetLimit(tenant_id, t);
    }
    return info;
}

bool QuotaService::CheckQuota(const TenantId& tenant_id, QuotaType type, int64_t amount,
                              const observability::TraceContext* /*ctx*/) {
    QuotaLimit limit = provider_->GetLimit(tenant_id, type);
    if (limit.IsUnlimited()) return true;
    // V1.0 OSS tracks no usage, so the comparison is against amount alone; Cloud V1
    // adds the persisted usage from platform.db (D3.5/Cloud-V1 wiring).
    return amount <= limit.limit;
}

void QuotaService::RecordUsage(const TenantId& /*tenant_id*/, QuotaType /*type*/,
                               int64_t /*amount*/, const observability::TraceContext* /*ctx*/) {
    // V1.0 OSS: no-op (topic 4 -- quota_usage table is not created). Cloud V1 persists
    // to platform.db here. cortrix_quota_record_total{result=noop} metric is D3.5.
}

Status QuotaService::UpdateQuotaOverride(const TenantId& tenant_id, QuotaType /*type*/,
                                         QuotaLimit /*new_limit*/, const AuthContext& /*admin_ctx*/,
                                         const observability::TraceContext* /*ctx*/) {
    if (!TenantExists(db_, tenant_id)) {
        return TenantStatus(TenantErrorCode::kTenantNotFound, tenant_id);
    }
    // V1.0 OSS: the platform.db quota_override table does not exist yet, so the
    // override is validated (tenant exists) and the persistence is a Cloud-V1
    // no-op. Wiring the write is Cloud V1 (StripePlanProvider).
    return Status::Ok();
}

}  // namespace cortrix::tenant
