// 🔒 jwt_auth_middleware — the gaps not covered by test_auth_middleware_jwt.cpp:
//   - a null `auth_` (uninitialized middleware) → CX_ERR_INTERNAL_ERROR / 500;
//   - api-key routing precedence: a cortrix_sk_* bearer AND an X-API-Key both
//     route to S6 (kNotFound), even when an X-API-Key would also be present;
//   - BuildMeResponseJson with MULTIPLE tenants + a CJK display_name round-trip.
// Pure logic; AuthService is only needed for the routing contrast and is built
// over an in-memory platform.db.
#include <gtest/gtest.h>

#include <string>

#include <nlohmann/json.hpp>

#include "cortrix/auth/auth_service.h"
#include "cortrix/auth/jwt_auth_middleware.h"
#include "cortrix/auth/password_utils.h"
#include "cortrix/auth/platform_db.h"
#include "cortrix/config/auth_config.h"
#include "cortrix/auth/user_info.h"

namespace cortrix::auth {
namespace {

// --- null auth_ → AuthenticateHeaders returns kInternalError (500), regardless
//     of a syntactically valid JWT-looking bearer. ---
TEST(JwtMiddlewareExtra, NullAuthServiceIsInternalError) {
    JwtAuthMiddleware mw(nullptr);
    auto r = mw.AuthenticateHeaders("Bearer aaa.bbb.ccc", "");
    EXPECT_FALSE(r.ok());
    EXPECT_EQ(r.status().code(), StatusCode::kInternal);
}

// --- null auth_ short-circuits BEFORE the api-key / missing-cred branches ---
TEST(JwtMiddlewareExtra, NullAuthServiceInternalEvenWithApiKey) {
    JwtAuthMiddleware mw(nullptr);
    // Even an api-key path hits the null guard first (it is checked at entry).
    auto r = mw.AuthenticateHeaders("", "some-api-key");
    EXPECT_FALSE(r.ok());
    EXPECT_EQ(r.status().code(), StatusCode::kInternal);
}

// --- A cortrix_sk_* bearer routes to S6 (kNotFound), and so does a plain
//     X-API-Key; both-present also routes to S6. ---
TEST(JwtMiddlewareExtra, ApiKeyBearerAndHeaderBothRouteToS6) {
    PlatformDb pdb;
    ASSERT_TRUE(pdb.Open(":memory:").ok());
    struct FakeHasher : IPasswordHasher {
        Result<std::string> Hash(const std::string& p) override { return std::string("f$") + p; }
        Result<bool> Verify(const std::string& p, const std::string& h) override {
            return h == (std::string("f$") + p);
        }
        int cost() const override { return 12; }
    } hasher;
    AuthService svc(pdb.db(), config::AuthConfig{}, &hasher,
                    "test-secret-key-at-least-32-bytes-long-for-testing");
    JwtAuthMiddleware mw(&svc);

    auto sk_bearer = mw.AuthenticateHeaders("Bearer cortrix_sk_abc123", "");
    EXPECT_EQ(sk_bearer.status().code(), StatusCode::kNotFound);

    auto both = mw.AuthenticateHeaders("Bearer cortrix_sk_abc123", "x-api-key-val");
    EXPECT_EQ(both.status().code(), StatusCode::kNotFound);

    auto header_only = mw.AuthenticateHeaders("", "x-api-key-val");
    EXPECT_EQ(header_only.status().code(), StatusCode::kNotFound);
}

// --- BuildMeResponseJson with MULTIPLE tenants preserves order + fields ---
TEST(JwtMiddlewareExtra, BuildMeResponseMultipleTenants) {
    UserInfo u;
    u.id = "usr_multi";
    u.email = "m@e.com";
    u.display_name = "Multi";
    u.email_verified = false;
    u.created_at = 1700000000;
    u.tenants.push_back({"tenant_a", "Alpha", "owner"});
    u.tenants.push_back({"tenant_b", "Beta", "admin"});
    u.tenants.push_back({"tenant_c", "Gamma", "viewer"});

    nlohmann::json j = nlohmann::json::parse(BuildMeResponseJson(u));
    ASSERT_TRUE(j["tenants"].is_array());
    ASSERT_EQ(j["tenants"].size(), 3u);
    EXPECT_EQ(j["tenants"][0]["id"], "tenant_a");
    EXPECT_EQ(j["tenants"][0]["role"], "owner");
    EXPECT_EQ(j["tenants"][1]["name"], "Beta");
    EXPECT_EQ(j["tenants"][2]["role"], "viewer");
    EXPECT_EQ(j["email_verified"], false);
}

// --- A CJK display_name round-trips through the JSON unchanged (UTF-8) ---
TEST(JwtMiddlewareExtra, BuildMeResponseCjkDisplayNameRoundTrip) {
    UserInfo u;
    u.id = "usr_cjk";
    u.email = "cjk@e.com";
    u.display_name = "\xE5\xBC\xA0\xE4\xB8\x89";  // CJK: "Zhang San" in UTF-8 bytes
    u.created_at = 1;

    nlohmann::json j = nlohmann::json::parse(BuildMeResponseJson(u));
    EXPECT_EQ(j["display_name"].get<std::string>(), std::string("\xE5\xBC\xA0\xE4\xB8\x89"));
}

}  // namespace
}  // namespace cortrix::auth
