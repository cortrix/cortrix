#include <gtest/gtest.h>

#include <string>

#include <nlohmann/json.hpp>

#include "cortrix/auth/auth_service.h"
#include "cortrix/auth/jwt_auth_middleware.h"
#include "cortrix/auth/password_utils.h"
#include "cortrix/auth/platform_db.h"
#include "cortrix/config/auth_config.h"

// P08 S4: JWT auth-middleware decision logic + /me response builder (§2.16 /
// §4.3 / §2.9). The httplib route mounting + CE/Cloud edition switch + protecting
// existing routes is cross-Feature server wiring → D3.5; this exercises the pure
// logic those will call.
namespace cortrix::auth {
namespace {

const std::string kSecret = "test-secret-key-at-least-32-bytes-long-for-testing";

class FakePasswordHasher : public IPasswordHasher {
public:
    Result<std::string> Hash(const std::string& p) override { return std::string("fake$") + p; }
    Result<bool> Verify(const std::string& p, const std::string& h) override {
        return h == (std::string("fake$") + p);
    }
    int cost() const override { return 12; }
};

class MiddlewareTest : public ::testing::Test {
protected:
    void SetUp() override { ASSERT_TRUE(pdb_.Open(":memory:").ok()); }
    AuthService Svc() { return AuthService(pdb_.db(), config::AuthConfig{}, &hasher_, kSecret); }
    PlatformDb pdb_;
    FakePasswordHasher hasher_;
};

// ---- ExtractBearer / IsApiKeyToken (pure helpers) ----

TEST(MiddlewareHelpersTest, ExtractBearer) {
    EXPECT_EQ(JwtAuthMiddleware::ExtractBearer("Bearer abc.def.ghi"), "abc.def.ghi");
    EXPECT_EQ(JwtAuthMiddleware::ExtractBearer(""), "");
    EXPECT_EQ(JwtAuthMiddleware::ExtractBearer("Basic xyz"), "");
    EXPECT_EQ(JwtAuthMiddleware::ExtractBearer("Bearer "), "");  // empty token
}

TEST(MiddlewareHelpersTest, IsApiKeyToken) {
    EXPECT_TRUE(JwtAuthMiddleware::IsApiKeyToken("cortrix_sk_abcdef"));
    EXPECT_FALSE(JwtAuthMiddleware::IsApiKeyToken("eyJhbGciOi"));  // a JWT
    EXPECT_FALSE(JwtAuthMiddleware::IsApiKeyToken(""));
}

// ---- AuthenticateHeaders decision logic (§4.3) ----

// `Middleware_ValidToken`: a valid Bearer JWT → AuthContext populated.
TEST_F(MiddlewareTest, ValidToken) {
    AuthService svc = Svc();
    ASSERT_TRUE(svc.Register("user@example.com", "Secure123!", "U").ok());
    auto login = svc.Login("user@example.com", "Secure123!");
    ASSERT_TRUE(login.ok());

    JwtAuthMiddleware mw(&svc);
    auto ctx = mw.AuthenticateHeaders("Bearer " + login.value().access_token, "");
    ASSERT_TRUE(ctx.ok()) << ctx.status().message();
    EXPECT_EQ(ctx.value().email, "user@example.com");
    EXPECT_FALSE(ctx.value().user_id.empty());
}

// `Middleware_NoHeader`: no Authorization + no X-API-Key → 401 CX_ERR_UNAUTHORIZED.
TEST_F(MiddlewareTest, NoHeader) {
    AuthService svc = Svc();
    JwtAuthMiddleware mw(&svc);
    auto ctx = mw.AuthenticateHeaders("", "");
    ASSERT_FALSE(ctx.ok());
    EXPECT_EQ(ctx.status().code(), StatusCode::kUnauthenticated);
    EXPECT_NE(ctx.status().message().find("CX_ERR_UNAUTHORIZED"), std::string::npos);
}

// `Middleware_InvalidToken`: a tampered/garbage Bearer → unauthorized.
TEST_F(MiddlewareTest, InvalidToken) {
    AuthService svc = Svc();
    JwtAuthMiddleware mw(&svc);
    auto ctx = mw.AuthenticateHeaders("Bearer not.a.valid.jwt", "");
    ASSERT_FALSE(ctx.ok());
    EXPECT_EQ(ctx.status().code(), StatusCode::kUnauthenticated);
}

// `Middleware_ApiKeyFallback`: an X-API-Key (or cortrix_sk_ bearer) is routed to
// the API-Key path (S6). Here AuthenticateHeaders reports "not the JWT path"
// (CX_ERR_NOT_FOUND) so D3.5 wiring delegates to ApiKeyService.
TEST_F(MiddlewareTest, ApiKeyFallbackDeferredToS6) {
    AuthService svc = Svc();
    JwtAuthMiddleware mw(&svc);
    auto via_header = mw.AuthenticateHeaders("", "some-api-key");
    EXPECT_FALSE(via_header.ok());
    EXPECT_EQ(via_header.status().code(), StatusCode::kNotFound);

    auto via_bearer = mw.AuthenticateHeaders("Bearer cortrix_sk_abc", "");
    EXPECT_FALSE(via_bearer.ok());
    EXPECT_EQ(via_bearer.status().code(), StatusCode::kNotFound);
}

// E2E (unit-level, no httplib): register → login → authenticate succeeds; after
// logout the same token is rejected (`E2E_RegisterLoginAccess` +
// `E2E_RegisterLoginLogoutAccess` from §S4, minus the httplib route).
TEST_F(MiddlewareTest, E2ERegisterLoginAccessThenLogout) {
    AuthService svc = Svc();
    ASSERT_TRUE(svc.Register("e2e@example.com", "Secure123!", "E2E").ok());
    auto login = svc.Login("e2e@example.com", "Secure123!");
    ASSERT_TRUE(login.ok());
    JwtAuthMiddleware mw(&svc);

    // access works.
    ASSERT_TRUE(mw.AuthenticateHeaders("Bearer " + login.value().access_token, "").ok());

    // logout → same token rejected (blacklisted).
    ASSERT_TRUE(svc.Logout(login.value().access_token).ok());
    auto after = mw.AuthenticateHeaders("Bearer " + login.value().access_token, "");
    ASSERT_FALSE(after.ok());
    EXPECT_NE(after.status().message().find("CX_ERR_AUTH_TOKEN_REVOKED"), std::string::npos);
}

// ---- /me response builder (§2.9) ----

TEST(MeResponseTest, BuildMeResponseJson) {
    UserInfo u;
    u.id = "usr_a1b2c3d4";
    u.email = "user@example.com";
    u.display_name = "Zhang San";
    u.email_verified = true;
    u.created_at = 1712035200;
    u.tenants.push_back({"tenant_001", "Workspace", "owner"});

    const std::string body = BuildMeResponseJson(u);
    nlohmann::json j = nlohmann::json::parse(body);
    EXPECT_EQ(j["id"], "usr_a1b2c3d4");
    EXPECT_EQ(j["email"], "user@example.com");
    EXPECT_EQ(j["display_name"], "Zhang San");
    EXPECT_EQ(j["email_verified"], true);
    EXPECT_EQ(j["created_at"], 1712035200);
    ASSERT_TRUE(j["tenants"].is_array());
    ASSERT_EQ(j["tenants"].size(), 1u);
    EXPECT_EQ(j["tenants"][0]["id"], "tenant_001");
    EXPECT_EQ(j["tenants"][0]["role"], "owner");
}

TEST(MeResponseTest, BuildMeResponseEmptyTenants) {
    UserInfo u;
    u.id = "usr_x";
    u.email = "x@y.com";
    u.display_name = "X";
    const std::string body = BuildMeResponseJson(u);
    nlohmann::json j = nlohmann::json::parse(body);
    EXPECT_TRUE(j["tenants"].is_array());
    EXPECT_EQ(j["tenants"].size(), 0u);  // empty until P09 wiring (D3.5)
}

}  // namespace
}  // namespace cortrix::auth
