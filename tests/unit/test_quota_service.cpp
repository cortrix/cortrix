// Tenancy -- QuotaService unit tests (UT36-40).
#include <gtest/gtest.h>

#include <memory>
#include <string>

#include <sqlite3.h>

#include "cortrix/auth/auth_context.h"
#include "cortrix/catalog/catalog_db.h"
#include "cortrix/tenant/i_plan_provider.h"
#include "cortrix/tenant/quota_service.h"
#include "cortrix/tenant/tenant_error.h"

namespace cortrix::tenant {
namespace {

using catalog::CatalogDb;

bool HasCx(const Status& st, const std::string& cx) {
    return !st.ok() && st.message().rfind(cx, 0) == 0;
}

AuthContext AdminCtx() {
    AuthContext c;
    c.user_id = "admin";
    c.permissions = kPermAdmin;
    return c;
}

// A mock provider with a fixed bounded limit (UT37) -- exercises the Cloud-V1 path.
class FixedPlanProvider : public IPlanProvider {
public:
    explicit FixedPlanProvider(int64_t limit) : limit_(limit) {}
    QuotaLimit GetLimit(const TenantId&, QuotaType) override { return {limit_}; }
private:
    int64_t limit_;
};

// --- UT36: UnlimitedPlanProvider -> CheckQuota always true ---
TEST(QuotaServiceTest, UnlimitedPlanProvider) {
    QuotaService svc(std::make_shared<UnlimitedPlanProvider>());
    EXPECT_TRUE(svc.CheckQuota("t1", QuotaType::MAX_NAMESPACES, 1'000'000));
    EXPECT_TRUE(svc.CheckQuota("t1", QuotaType::MAX_QPS, 999999));
}

// --- UT37: a bounded mock provider enforces the limit ---
TEST(QuotaServiceTest, BoundedPlanProvider_Enforces) {
    QuotaService svc(std::make_shared<FixedPlanProvider>(5));
    EXPECT_TRUE(svc.CheckQuota("t1", QuotaType::MAX_NAMESPACES, 5));   // == limit ok
    EXPECT_FALSE(svc.CheckQuota("t1", QuotaType::MAX_NAMESPACES, 6));  // over limit
}

// --- UT38: GetQuota tenant not found ---
TEST(QuotaServiceTest, GetQuota_TenantNotFound) {
    CatalogDb catalog;
    ASSERT_TRUE(catalog.Open(":memory:").ok());
    QuotaService svc(std::make_shared<UnlimitedPlanProvider>(), catalog.db());
    auto r = svc.GetQuota("t-missing", AdminCtx());
    ASSERT_FALSE(r.ok());
    EXPECT_TRUE(HasCx(r.status(), "CX_ERR_TENANT_NOT_FOUND"));
}

TEST(QuotaServiceTest, GetQuota_UnlimitedReportsAllTypes) {
    CatalogDb catalog;
    ASSERT_TRUE(catalog.Open(":memory:").ok());
    ASSERT_EQ(sqlite3_exec(catalog.db(),
                           "INSERT INTO tenants(tenant_id, type, name, created_at) "
                           "VALUES('t1','personal','t',0)",
                           nullptr, nullptr, nullptr), SQLITE_OK);
    QuotaService svc(std::make_shared<UnlimitedPlanProvider>(), catalog.db());
    auto r = svc.GetQuota("t1", AdminCtx());
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_EQ(r->tenant_id, "t1");
    EXPECT_EQ(r->limits.size(), 5u);   // all five QuotaTypes reported
    EXPECT_EQ(r->usage.size(), 5u);
    for (const auto& [type, lim] : r->limits) EXPECT_TRUE(lim.IsUnlimited());
    for (const auto& [type, u] : r->usage) EXPECT_EQ(u, 0);  // V1.0 OSS usage = 0
}

// --- UT40: RecordUsage is a no-op in V1.0 OSS (no quota_usage table) ---
TEST(QuotaServiceTest, RecordUsage_NoOp_V1OSS) {
    CatalogDb catalog;
    ASSERT_TRUE(catalog.Open(":memory:").ok());
    QuotaService svc(std::make_shared<UnlimitedPlanProvider>(), catalog.db());
    // Must not throw / must not create a quota_usage table.
    svc.RecordUsage("t1", QuotaType::MAX_NAMESPACES, 3);

    sqlite3_stmt* s = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(
                  catalog.db(),
                  "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='quota_usage'",
                  -1, &s, nullptr), SQLITE_OK);
    ASSERT_EQ(sqlite3_step(s), SQLITE_ROW);
    EXPECT_EQ(sqlite3_column_int(s, 0), 0);  // table never created
    sqlite3_finalize(s);
}

TEST(QuotaServiceTest, UpdateQuotaOverride_TenantNotFound) {
    CatalogDb catalog;
    ASSERT_TRUE(catalog.Open(":memory:").ok());
    QuotaService svc(std::make_shared<UnlimitedPlanProvider>(), catalog.db());
    auto st = svc.UpdateQuotaOverride("t-missing", QuotaType::MAX_QPS, {100}, AdminCtx());
    ASSERT_FALSE(st.ok());
    EXPECT_TRUE(HasCx(st, "CX_ERR_TENANT_NOT_FOUND"));
}

}  // namespace
}  // namespace cortrix::tenant
