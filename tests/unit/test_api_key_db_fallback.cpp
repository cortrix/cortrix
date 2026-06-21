// 🔒 api_key_auth.cpp db_keys_ fallback (l.38-47) — unit-pin the over-grant path.
// On a static-config MISS, Authenticate() falls through to the platform.db-backed
// ApiKeyService::ValidateApiKey; a valid DB key yields an ADMIN AuthContext with
// tenant_id="default" (CE single-user model). A regression here silently
// over-grants, so each branch is asserted in isolation: DB-key valid → admin ctx,
// DB-key invalid/revoked → Unauthenticated, static-config hit short-circuits DB,
// and no-service-bound preserves the original "Invalid API key".
#include <gtest/gtest.h>

#include <sqlite3.h>

#include <string>

#include "cortrix/auth/api_key_auth.h"
#include "cortrix/auth/api_key_service.h"
#include "cortrix/auth/platform_db.h"

namespace cortrix {
namespace {

using auth::ApiKeyService;
using auth::PlatformDb;

class ApiKeyDbFallbackTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(pdb_.Open(":memory:").ok());
        svc_ = std::make_unique<ApiKeyService>(pdb_.db());
        SeedUser("usr_owner");
    }

    void SeedUser(const std::string& id) {
        const std::string sql =
            "INSERT INTO users(id,email,password_hash,display_name,email_verified,"
            "status,login_attempts,created_at,updated_at) VALUES('" + id +
            "','o@e.com','h','O',1,'active',0,1,1)";
        ASSERT_EQ(sqlite3_exec(pdb_.db(), sql.c_str(), nullptr, nullptr, nullptr), SQLITE_OK);
    }

    PlatformDb pdb_;
    std::unique_ptr<ApiKeyService> svc_;
    ApiKeyAuth auth_;
};

// --- A valid DB key (no static-config match) → admin ctx, tenant=default ---
TEST_F(ApiKeyDbFallbackTest, ValidDbKeyGrantsAdminDefaultTenant) {
    auto created = svc_->CreateApiKey("usr_owner", "agent-1");
    ASSERT_TRUE(created.ok()) << created.status().message();

    auth_.SetApiKeyService(svc_.get());  // no LoadKeys → static map is empty

    AuthContext ctx;
    Status s = auth_.Authenticate(created.value().plaintext, &ctx);
    ASSERT_TRUE(s.ok()) << s.message();
    EXPECT_EQ(ctx.tenant_id, "default");           // CE single-tenant fallback
    EXPECT_EQ(ctx.user_id, "usr_owner");           // owning user_id from the row
    EXPECT_TRUE(ctx.agent_id.empty());
    EXPECT_EQ(ctx.permissions, kPermRead | kPermWrite | kPermAdmin);  // FULL admin
    EXPECT_TRUE(ctx.is_admin());
}

// --- An unknown key with the service bound → still Unauthenticated ---
TEST_F(ApiKeyDbFallbackTest, UnknownKeyWithServiceBoundIsUnauthenticated) {
    auth_.SetApiKeyService(svc_.get());
    AuthContext ctx;
    Status s = auth_.Authenticate("cortrix_sk_does_not_exist", &ctx);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kUnauthenticated);
    EXPECT_EQ(s.message(), "Invalid API key");
}

// --- A revoked DB key is rejected by the fallback (ValidateApiKey fails) ---
TEST_F(ApiKeyDbFallbackTest, RevokedDbKeyRejected) {
    auto created = svc_->CreateApiKey("usr_owner", "agent-rev");
    ASSERT_TRUE(created.ok());
    ASSERT_TRUE(svc_->RevokeApiKey(created.value().info.id).ok());

    auth_.SetApiKeyService(svc_.get());
    AuthContext ctx;
    Status s = auth_.Authenticate(created.value().plaintext, &ctx);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kUnauthenticated);
}

// --- An expired DB key is rejected by the fallback ---
TEST_F(ApiKeyDbFallbackTest, ExpiredDbKeyRejected) {
    // expires_at_ms in the past → ValidateApiKey treats it as expired.
    auto created = svc_->CreateApiKey("usr_owner", "agent-exp", /*expires_at_ms=*/1);
    ASSERT_TRUE(created.ok());

    auth_.SetApiKeyService(svc_.get());
    AuthContext ctx;
    Status s = auth_.Authenticate(created.value().plaintext, &ctx);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kUnauthenticated);
}

// --- A static-config hit short-circuits the DB fallback (DB never consulted) ---
TEST_F(ApiKeyDbFallbackTest, StaticConfigHitShortCircuitsDb) {
    const std::string static_key = "static-config-key";
    ApiKeyConfig kc;
    kc.key_hash = ApiKeyAuth::HashKey(static_key);
    kc.tenant_id = "tenant_static";
    kc.permissions = kPermRead;  // NOT admin — proves the static row was used
    kc.expires_at = 0;
    auth_.LoadKeys({kc});
    auth_.SetApiKeyService(svc_.get());

    AuthContext ctx;
    Status s = auth_.Authenticate(static_key, &ctx);
    ASSERT_TRUE(s.ok());
    EXPECT_EQ(ctx.tenant_id, "tenant_static");  // static, not the "default" fallback
    EXPECT_EQ(ctx.permissions, kPermRead);      // read-only, not the admin over-grant
    EXPECT_FALSE(ctx.is_admin());
}

// --- Without a bound service, a config miss preserves the original error ---
TEST_F(ApiKeyDbFallbackTest, NoServiceBoundMissIsInvalidApiKey) {
    // db_keys_ is nullptr (SetApiKeyService never called).
    auto created = svc_->CreateApiKey("usr_owner", "agent-unbound");
    ASSERT_TRUE(created.ok());
    AuthContext ctx;
    Status s = auth_.Authenticate(created.value().plaintext, &ctx);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kUnauthenticated);
    EXPECT_EQ(s.message(), "Invalid API key");  // no fallback attempted
}

}  // namespace
}  // namespace cortrix
