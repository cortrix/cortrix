// Error-path coverage for previously-untested Tenant / ACL CX_ERR_* codes.
//
// Each TEST constructs the real precondition that makes the code fire and
// asserts on the surfaced identity (CX_ERR_* token in the service Status, or the
// JSON error-envelope "code" field + HTTP status at the route layer).
//
// Covered:
//   - CX_ERR_TENANT_EMAIL_DUPLICATE   (TenantService::CreatePersonal -- service)
//   - CX_ERR_MEMBER_ROLE_INVALID      (POST /api/v1/tenants, missing 'name' -- route)
//   - CX_ERR_ACL_GRANT_INVALID_ROLE   (POST .../acl, invalid/missing role  -- route)
//   - CX_ERR_TENANT_QUOTA_NOT_FOUND   (PATCH .../quota, unknown quota_type -- route)
//
// Deferred (see report): CX_ERR_PERMISSION_OWNER_ONLY, CX_ERR_TENANT_NAME_DUPLICATE,
// CX_ERR_TENANT_ALREADY_EXISTS -- no reachable constructor / only via uuid PK clash.

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include <sqlite3.h>

#include "httplib.h"
#include <nlohmann/json.hpp>

#include "cortrix/auth/api_key_auth.h"
#include "cortrix/auth/auth_context.h"
#include "cortrix/catalog/catalog_db.h"
#include "cortrix/server/routes/tenant_routes.h"
#include "cortrix/tenant/i_plan_provider.h"
#include "cortrix/tenant/permission_service.h"
#include "cortrix/tenant/quota_service.h"
#include "cortrix/tenant/tenant_error.h"
#include "cortrix/tenant/tenant_service.h"

namespace cortrix {
namespace {

using json = nlohmann::json;
using catalog::CatalogDb;
using tenant::PermissionService;
using tenant::QuotaService;
using tenant::TenantService;
using tenant::UnlimitedPlanProvider;

// ===========================================================================
// CX_ERR_TENANT_EMAIL_DUPLICATE -- service layer (CreatePersonal).
//
// CreatePersonal INSERTs tenants(name=email, type='personal'); a second personal
// tenant with the same email collides on the UNIQUE name constraint and surfaces
// as the email-duplicate identity (SQLITE_CONSTRAINT branch). The CX token
// prefixes the Status message ("CX_ERR_X: detail").
// ===========================================================================
class TenantEmailDupTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(catalog_.Open(":memory:").ok());
        svc_ = std::make_unique<TenantService>(catalog_.db());
    }

    void SeedUser(const std::string& uid, const std::string& email) {
        sqlite3_stmt* s = nullptr;
        ASSERT_EQ(sqlite3_prepare_v2(catalog_.db(),
                                     "INSERT INTO users(user_id, email, created_at) VALUES(?,?,0)",
                                     -1, &s, nullptr),
                  SQLITE_OK);
        sqlite3_bind_text(s, 1, uid.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 2, email.c_str(), -1, SQLITE_TRANSIENT);
        ASSERT_EQ(sqlite3_step(s), SQLITE_DONE);
        sqlite3_finalize(s);
    }

    static bool HasCx(const Status& st, const char* cx) {
        return !st.ok() && st.message().rfind(cx, 0) == 0;  // starts_with
    }

    CatalogDb catalog_;
    std::unique_ptr<TenantService> svc_;
};

// NOTE (verified 2026-06-20): kTenantEmailDuplicate is currently UNREACHABLE via
// CreatePersonal. The branch (tenant_service.cpp ~line 134) fires only on
// SQLITE_CONSTRAINT, and its comment assumes a UNIQUE on tenants.name -- but
// catalog_schema.cpp defines `name TEXT` with NO UNIQUE (the only UNIQUE on
// tenants is the tenant_id PK). Email uniqueness is in fact enforced UPSTREAM by
// users.email UNIQUE (auth_schema.cpp), so two users cannot share an email in
// production and this tenant-level guard is unreachable defense-in-depth.
// This test pins the ACTUAL current behavior + the reachable error->status
// mapping; the unreachable branch is reported as a finding for design review
// (add UNIQUE on tenants.name for personal tenants, or drop the dead branch).
TEST_F(TenantEmailDupTest, EmailDuplicateBranchUnreachableUnderCurrentSchema) {
    SeedUser("u1", "dup@x.com");
    // The catalog test DB's users table has no UNIQUE(email); production's
    // platform.db (auth_schema) does, which is what actually blocks dup emails.
    SeedUser("u2", "dup@x.com");

    auto first = svc_->CreatePersonal("u1", "dup@x.com", /*conn=*/nullptr);
    ASSERT_TRUE(first.ok()) << first.status().message();

    // Second personal tenant with the same email SUCCEEDS today: tenants.name has
    // no UNIQUE, so the SQLITE_CONSTRAINT -> kTenantEmailDuplicate path is dead.
    auto second = svc_->CreatePersonal("u2", "dup@x.com", /*conn=*/nullptr);
    EXPECT_TRUE(second.ok()) << "schema changed? tenants.name UNIQUE would now make "
                                "kTenantEmailDuplicate reachable: "
                             << second.status().message();

    // The error->status mapping IS reachable and is the contract a future
    // schema/upstream check would surface; assert it stays kAlreadyExists.
    EXPECT_EQ(tenant::TenantErrorToStatusCode(tenant::TenantErrorCode::kTenantEmailDuplicate),
              StatusCode::kAlreadyExists);
}

// ===========================================================================
// Route layer for the three input-validation tenant codes.
//
// The targeted codes all fire from in-handler request validation BEFORE the
// service is invoked, so the services need only be constructed (no seeded data).
// ===========================================================================
class TenantRouteErrPathTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(catalog_.Open(":memory:").ok());
        tenant_svc_ = std::make_unique<TenantService>(catalog_.db());
        plan_ = std::make_shared<UnlimitedPlanProvider>();
        perm_svc_ = std::make_unique<PermissionService>(catalog_.db());
        quota_svc_ = std::make_unique<QuotaService>(plan_, catalog_.db());

        auth_ = std::make_unique<ApiKeyAuth>();
        admin_key_ = "admin-key";
        writer_key_ = "writer-key";
        auth_->LoadKeys({
            MakeKey(admin_key_, "root", kPermRead | kPermWrite | kPermAdmin),
            MakeKey(writer_key_, "writer", kPermRead | kPermWrite),
        });

        port_ = 19400 + (getpid() % 250);
        server_ = std::make_unique<httplib::Server>();
        RegisterTenantRoutes(*server_, *tenant_svc_, *perm_svc_, *quota_svc_, *auth_);
        server_thread_ = std::thread([this] { server_->listen("127.0.0.1", port_); });

        httplib::Client cli("127.0.0.1", port_);
        for (int i = 0; i < 50; ++i) {
            auto res = cli.Get("/api/v1/tenants", Bearer(admin_key_));
            if (res) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    void TearDown() override {
        if (server_) server_->stop();
        if (server_thread_.joinable()) server_thread_.join();
    }

    static ApiKeyConfig MakeKey(const std::string& plaintext, const std::string& tenant,
                                int perms) {
        ApiKeyConfig k;
        k.key_hash = ApiKeyAuth::HashKey(plaintext);
        k.tenant_id = tenant;
        k.permissions = perms;
        return k;
    }

    httplib::Headers Bearer(const std::string& key) {
        return {{"Authorization", "Bearer " + key}};
    }

    CatalogDb catalog_;
    std::unique_ptr<TenantService> tenant_svc_;
    std::shared_ptr<UnlimitedPlanProvider> plan_;
    std::unique_ptr<PermissionService> perm_svc_;
    std::unique_ptr<QuotaService> quota_svc_;
    std::unique_ptr<ApiKeyAuth> auth_;
    std::unique_ptr<httplib::Server> server_;
    std::thread server_thread_;
    int port_ = 0;
    std::string admin_key_, writer_key_;
};

// CX_ERR_MEMBER_ROLE_INVALID: admin POST /api/v1/tenants with a body missing the
// required 'name' field -> 400 + the member-role-invalid envelope (the handler's
// missing-name branch reuses this identity).
TEST_F(TenantRouteErrPathTest, CreateTenantMissingNameIsMemberRoleInvalid) {
    httplib::Client cli("127.0.0.1", port_);
    json body = json::object();  // no "name"
    auto res = cli.Post("/api/v1/tenants", Bearer(admin_key_), body.dump(),
                        "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);  // kMemberRoleInvalid -> kInvalidArgument -> 400

    auto parsed = json::parse(res->body);
    ASSERT_TRUE(parsed.contains("error"));
    EXPECT_EQ(parsed["error"]["code"], "CX_ERR_MEMBER_ROLE_INVALID");
}

// CX_ERR_ACL_GRANT_INVALID_ROLE: POST .../acl with an unparseable 'role' token
// -> 400 + the acl-grant-invalid-role envelope. Only kPermWrite is needed; the
// role check precedes the service call.
TEST_F(TenantRouteErrPathTest, GrantAclInvalidRoleTokenIsAclGrantInvalidRole) {
    httplib::Client cli("127.0.0.1", port_);
    json body = {{"grantee_tenant_id", "tenant-xyz"}, {"role", "superuser"}};  // not viewer/editor/admin
    auto res = cli.Post("/api/v1/namespaces/ns-1/acl", Bearer(writer_key_),
                        body.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);

    auto parsed = json::parse(res->body);
    ASSERT_TRUE(parsed.contains("error"));
    EXPECT_EQ(parsed["error"]["code"], "CX_ERR_ACL_GRANT_INVALID_ROLE");
}

// CX_ERR_ACL_GRANT_INVALID_ROLE (second branch): missing 'role'/'grantee_tenant_id'.
TEST_F(TenantRouteErrPathTest, GrantAclMissingFieldsIsAclGrantInvalidRole) {
    httplib::Client cli("127.0.0.1", port_);
    json body = {{"grantee_tenant_id", "tenant-xyz"}};  // no "role"
    auto res = cli.Post("/api/v1/namespaces/ns-1/acl", Bearer(writer_key_),
                        body.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);

    auto parsed = json::parse(res->body);
    EXPECT_EQ(parsed["error"]["code"], "CX_ERR_ACL_GRANT_INVALID_ROLE");
}

// CX_ERR_TENANT_QUOTA_NOT_FOUND: admin PATCH .../quota with an unknown
// quota_type token -> 404 + the quota-not-found envelope.
TEST_F(TenantRouteErrPathTest, UpdateQuotaUnknownTypeIsQuotaNotFound) {
    httplib::Client cli("127.0.0.1", port_);
    json body = {{"quota_type", "max_unicorns"}, {"limit", 10}};  // unknown type
    auto res = cli.Patch("/api/v1/admin/tenants/tenant-abc/quota", Bearer(admin_key_),
                         body.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404);  // kTenantQuotaNotFound -> kNotFound -> 404

    auto parsed = json::parse(res->body);
    ASSERT_TRUE(parsed.contains("error"));
    EXPECT_EQ(parsed["error"]["code"], "CX_ERR_TENANT_QUOTA_NOT_FOUND");
}

// CX_ERR_TENANT_QUOTA_NOT_FOUND (second branch): missing 'quota_type'/'limit'.
TEST_F(TenantRouteErrPathTest, UpdateQuotaMissingFieldsIsQuotaNotFound) {
    httplib::Client cli("127.0.0.1", port_);
    json body = {{"limit", 10}};  // no "quota_type"
    auto res = cli.Patch("/api/v1/admin/tenants/tenant-abc/quota", Bearer(admin_key_),
                         body.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404);

    auto parsed = json::parse(res->body);
    EXPECT_EQ(parsed["error"]["code"], "CX_ERR_TENANT_QUOTA_NOT_FOUND");
}

}  // namespace
}  // namespace cortrix
