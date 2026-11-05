// 🔒 auth_middleware WithAuth — header precedence + obs-context behavior.
// Drives the WithAuth-returned httplib handler directly (no socket): constructs
// Request/Response in-process and asserts:
//   - Authorization: Bearer wins over X-API-Key when both are present;
//   - an exactly-"Bearer " (empty remainder) falls through to X-API-Key;
//   - missing both → 401 UNAUTHORIZED;
//   - invalid obs-context headers set X-Cortrix-Header-Warning but STILL serve
//     (pure-ADD must never reject);
//   - the no-auth path grants admin and still installs the obs context.
#include <gtest/gtest.h>

#include <string>

#include <nlohmann/json.hpp>

#include "httplib.h"

#include "cortrix/auth/api_key_auth.h"
#include "cortrix/auth/auth_middleware.h"
#include "cortrix/auth/request_credential.h"
#include "cortrix/middleware/rate_limiter.h"
#include "cortrix/server/request_context.h"

namespace cortrix {
namespace {

using json = nlohmann::json;

// A handler that records the AuthContext it was called with + that it ran.
struct Capture {
    bool called = false;
    AuthContext ctx;
};

HttpHandler RecordingHandler(Capture* cap) {
    return [cap](const httplib::Request&, httplib::Response& res, const RequestContext& rctx) {
        cap->called = true;
        cap->ctx = rctx.auth;
        res.status = 200;
        res.set_content("{\"ok\":true}", "application/json");
    };
}

// Populate an ApiKeyAuth (non-copyable/-movable) in place with one static key
// → known tenant + permissions.
void MakeAuth(ApiKeyAuth& auth, const std::string& key, const std::string& tenant, int perms) {
    ApiKeyConfig kc;
    kc.key_hash = ApiKeyAuth::HashKey(key);
    kc.tenant_id = tenant;
    kc.permissions = perms;
    kc.expires_at = 0;
    auth.LoadKeys({kc});
}

// --- Authorization: Bearer wins over X-API-Key when BOTH are present ---
TEST(AuthMiddlewarePrecedence, BearerWinsOverApiKeyHeader) {
    // Two distinct keys → distinct tenants; assert the Bearer one was used.
    ApiKeyAuth auth;
    ApiKeyConfig bearer_kc;
    bearer_kc.key_hash = ApiKeyAuth::HashKey("bearer-key");
    bearer_kc.tenant_id = "tenant_bearer";
    bearer_kc.permissions = kPermRead;
    ApiKeyConfig hdr_kc;
    hdr_kc.key_hash = ApiKeyAuth::HashKey("xapikey-key");
    hdr_kc.tenant_id = "tenant_header";
    hdr_kc.permissions = kPermRead;
    auth.LoadKeys({bearer_kc, hdr_kc});

    Capture cap;
    auto h = WithAuth(auth, kPermRead, RecordingHandler(&cap));

    httplib::Request req;
    req.set_header("Authorization", "Bearer bearer-key");
    req.set_header("X-API-Key", "xapikey-key");
    httplib::Response res;
    h(req, res);

    ASSERT_TRUE(cap.called);
    EXPECT_EQ(cap.ctx.tenant_id, "tenant_bearer");  // Bearer token won
}

// --- Exactly "Bearer " (empty remainder) falls through to X-API-Key ---
TEST(AuthMiddlewarePrecedence, EmptyBearerFallsThroughToApiKey) {
    ApiKeyAuth auth;
    MakeAuth(auth, "xapikey-only", "tenant_xapi", kPermRead);

    Capture cap;
    auto h = WithAuth(auth, kPermRead, RecordingHandler(&cap));

    httplib::Request req;
    req.set_header("Authorization", "Bearer ");  // empty token after prefix
    req.set_header("X-API-Key", "xapikey-only");
    httplib::Response res;
    h(req, res);

    ASSERT_TRUE(cap.called);
    EXPECT_EQ(cap.ctx.tenant_id, "tenant_xapi");  // fell through to X-API-Key
}

// --- Neither header present → 401 UNAUTHORIZED, handler NOT invoked ---
TEST(AuthMiddlewarePrecedence, MissingBothHeaders401) {
    ApiKeyAuth auth;
    MakeAuth(auth, "k", "t", kPermRead);
    Capture cap;
    auto h = WithAuth(auth, kPermRead, RecordingHandler(&cap));

    httplib::Request req;  // no auth headers
    httplib::Response res;
    h(req, res);

    EXPECT_FALSE(cap.called);
    EXPECT_EQ(res.status, 401);
    auto body = json::parse(res.body);
    EXPECT_EQ(body["error"]["code"], "UNAUTHORIZED");
}

// --- An invalid token (no matching key) → 401 INVALID_API_KEY ---
TEST(AuthMiddlewarePrecedence, InvalidTokenRejected) {
    ApiKeyAuth auth;
    MakeAuth(auth, "good-key", "t", kPermRead);
    Capture cap;
    auto h = WithAuth(auth, kPermRead, RecordingHandler(&cap));

    httplib::Request req;
    req.set_header("X-API-Key", "wrong-key");
    httplib::Response res;
    h(req, res);

    EXPECT_FALSE(cap.called);
    EXPECT_EQ(res.status, 401);
    auto body = json::parse(res.body);
    EXPECT_EQ(body["error"]["code"], "INVALID_API_KEY");
}

// --- Invalid obs-context headers warn (X-Cortrix-Header-Warning) but STILL
//     serve the request (pure-ADD must never reject). ---
TEST(AuthMiddlewarePrecedence, InvalidObsHeadersWarnNotReject) {
    ApiKeyAuth auth;
    MakeAuth(auth, "obs-key", "tenant_obs", kPermRead);
    Capture cap;
    auto h = WithAuth(auth, kPermRead, RecordingHandler(&cap));

    httplib::Request req;
    req.set_header("X-API-Key", "obs-key");
    // A malformed session id (contains spaces + '!' — outside the [a-zA-Z0-9_.:/-]
    // whitelist) is rejected by the obs middleware validation → warning header,
    // but the request still serves (pure-ADD never rejects).
    req.set_header("X-Session-Id", "bad session id!");
    httplib::Response res;
    h(req, res);

    ASSERT_TRUE(cap.called);  // served despite the bad header
    EXPECT_EQ(res.status, 200);
    EXPECT_EQ(res.get_header_value("X-Cortrix-Header-Warning"), "invalid-format");
}

// --- A valid X-Session-Id does NOT set the warning header ---
TEST(AuthMiddlewarePrecedence, ValidObsHeaderNoWarning) {
    ApiKeyAuth auth;
    MakeAuth(auth, "obs-key2", "tenant_obs2", kPermRead);
    Capture cap;
    auto h = WithAuth(auth, kPermRead, RecordingHandler(&cap));

    httplib::Request req;
    req.set_header("X-API-Key", "obs-key2");
    req.set_header("X-Session-Id", "sess-0123456789abcdef");
    httplib::Response res;
    h(req, res);

    ASSERT_TRUE(cap.called);
    EXPECT_EQ(res.status, 200);
    EXPECT_TRUE(res.get_header_value("X-Cortrix-Header-Warning").empty());
}

// --- Auth disabled: WithAuth grants admin + serves without any credentials ---
TEST(AuthMiddlewarePrecedence, AuthDisabledGrantsAdmin) {
    ApiKeyAuth auth;
    auth.set_enabled(false);
    Capture cap;
    auto h = WithAuth(auth, kPermAdmin, RecordingHandler(&cap));

    httplib::Request req;  // no credentials
    httplib::Response res;
    h(req, res);

    ASSERT_TRUE(cap.called);
    EXPECT_EQ(cap.ctx.user_id, "anonymous");
    EXPECT_EQ(cap.ctx.permissions, kPermRead | kPermWrite | kPermAdmin);
}

// --- A permission shortfall → 403 FORBIDDEN, handler NOT invoked ---
TEST(AuthMiddlewarePrecedence, InsufficientPermission403) {
    ApiKeyAuth auth;
    MakeAuth(auth, "ro-key", "tenant_ro", kPermRead);  // read-only key
    Capture cap;
    auto h = WithAuth(auth, kPermWrite, RecordingHandler(&cap));  // route needs write

    httplib::Request req;
    req.set_header("X-API-Key", "ro-key");
    httplib::Response res;
    h(req, res);

    EXPECT_FALSE(cap.called);
    EXPECT_EQ(res.status, 403);
    auto body = json::parse(res.body);
    EXPECT_EQ(body["error"]["code"], "FORBIDDEN");
}

// --- RequestCredential: the precedence WithAuth applies ---
TEST(AuthMiddlewarePrecedence, RequestCredentialReadsBearerThenApiKey) {
    httplib::Request both;
    both.set_header("Authorization", "Bearer bearer-key");
    both.set_header("X-API-Key", "xapikey-key");
    EXPECT_EQ(RequestCredential(both), "bearer-key");

    httplib::Request empty_bearer;
    empty_bearer.set_header("Authorization", "Bearer ");
    empty_bearer.set_header("X-API-Key", "xapikey-key");
    EXPECT_EQ(RequestCredential(empty_bearer), "xapikey-key");

    httplib::Request other_scheme;
    other_scheme.set_header("Authorization", "Basic dXNlcjpwYXNz");
    other_scheme.set_header("X-API-Key", "xapikey-key");
    EXPECT_EQ(RequestCredential(other_scheme), "xapikey-key");

    EXPECT_EQ(RequestCredential(httplib::Request{}), "");
}

// --- The rate limiter counts the key WithAuth authenticates ---
// A caller that keeps one Bearer key and sends a different X-API-Key each time is
// authenticated with the Bearer key every time, and the per-API-key bucket of that
// key runs out: the X-API-Key values do not open fresh buckets.
TEST(AuthMiddlewarePrecedence, RateLimiterCountsTheBearerKeyDespiteRotatingApiKeyHeaders) {
    ApiKeyAuth auth;
    MakeAuth(auth, "bearer-key", "tenant_bearer", kPermRead);
    Capture cap;
    auto handler = WithAuth(auth, kPermRead, RecordingHandler(&cap));

    const middleware::RateLimitRule unreachable{0.0, 1e9, "60s", 1000000000};
    middleware::RateLimiter limiter(unreachable,
                                    middleware::RateLimitRule{0.0, 2.0, "60s", 2},
                                    unreachable);
    auto limit = middleware::RateLimitHandler(limiter);

    for (int i = 0; i < 3; ++i) {
        httplib::Request req;
        req.remote_addr = "10.0.0.7";
        req.set_header("Authorization", "Bearer bearer-key");
        req.set_header("X-API-Key", "rotating-" + std::to_string(i));
        httplib::Response res;
        if (limit(req, res) == httplib::Server::HandlerResponse::Handled) {
            ASSERT_EQ(i, 2) << "limited too early";
            EXPECT_EQ(res.status, 429);
            const json body = json::parse(res.body);
            EXPECT_EQ(body["error"]["code"], "CX_ERR_RATE_LIMITED");
            EXPECT_EQ(body["error"]["structured_data"]["stratum"], "per_api_key");
            return;
        }
        cap = Capture{};
        handler(req, res);
        ASSERT_TRUE(cap.called) << "request " << i;
        EXPECT_EQ(cap.ctx.tenant_id, "tenant_bearer");  // authenticated with the Bearer key
    }
    FAIL() << "the third request with the same Bearer key was not rate limited";
}

}  // namespace
}  // namespace cortrix
