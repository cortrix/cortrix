#include <gtest/gtest.h>
#include "cortrix/auth/api_key_auth.h"

namespace cortrix {
namespace {

class ApiKeyAuthTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Pre-computed: SHA-256("test-api-key-123")
        test_key_ = "test-api-key-123";
        test_hash_ = ApiKeyAuth::HashKey(test_key_);

        ApiKeyConfig kc;
        kc.key_hash = test_hash_;
        kc.tenant_id = "tenant_001";
        kc.permissions = kPermRead | kPermWrite | kPermAdmin;
        kc.expires_at = 0;  // never expires
        kc.allowed_namespaces = {};

        auth_.LoadKeys({kc});
    }

    ApiKeyAuth auth_;
    std::string test_key_;
    std::string test_hash_;
};

TEST_F(ApiKeyAuthTest, HashKeyKnownVector) {
    // SHA-256 of "test" should be a known value
    std::string hash = ApiKeyAuth::HashKey("test");
    EXPECT_EQ(hash.size(), 64u);
    EXPECT_EQ(hash, "9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08");
}

TEST_F(ApiKeyAuthTest, HashKeyConsistency) {
    std::string h1 = ApiKeyAuth::HashKey("hello");
    std::string h2 = ApiKeyAuth::HashKey("hello");
    EXPECT_EQ(h1, h2);
    EXPECT_EQ(h1.size(), 64u);
}

TEST_F(ApiKeyAuthTest, HashKeyDifferentInputs) {
    std::string h1 = ApiKeyAuth::HashKey("key1");
    std::string h2 = ApiKeyAuth::HashKey("key2");
    EXPECT_NE(h1, h2);
}

TEST_F(ApiKeyAuthTest, AuthenticateValidKey) {
    AuthContext ctx;
    Status s = auth_.Authenticate(test_key_, &ctx);
    EXPECT_TRUE(s.ok());
    EXPECT_EQ(ctx.tenant_id, "tenant_001");
    EXPECT_EQ(ctx.permissions, kPermRead | kPermWrite | kPermAdmin);
}

TEST_F(ApiKeyAuthTest, AuthenticateInvalidKey) {
    AuthContext ctx;
    Status s = auth_.Authenticate("wrong-key", &ctx);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kUnauthenticated);
    EXPECT_EQ(s.message(), "Invalid API key");
}

TEST_F(ApiKeyAuthTest, AuthenticateEmptyKey) {
    AuthContext ctx;
    Status s = auth_.Authenticate("", &ctx);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kUnauthenticated);
    EXPECT_EQ(s.message(), "API key is empty");
}

TEST_F(ApiKeyAuthTest, AuthenticateExpiredKey) {
    std::string expired_key = "expired-key-456";
    std::string expired_hash = ApiKeyAuth::HashKey(expired_key);

    ApiKeyConfig kc;
    kc.key_hash = expired_hash;
    kc.tenant_id = "tenant_002";
    kc.permissions = 7;
    kc.expires_at = 1;  // expired (unix timestamp 1 = 1970)

    auth_.LoadKeys({kc});

    AuthContext ctx;
    Status s = auth_.Authenticate(expired_key, &ctx);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kUnauthenticated);
    EXPECT_EQ(s.message(), "API key expired");
}

TEST_F(ApiKeyAuthTest, AuthenticateNonExpiredKey) {
    // expires_at = 0 means never expires
    AuthContext ctx;
    Status s = auth_.Authenticate(test_key_, &ctx);
    EXPECT_TRUE(s.ok());
}

TEST_F(ApiKeyAuthTest, AuthenticateWithNamespaces) {
    std::string key = "ns-key";
    std::string hash = ApiKeyAuth::HashKey(key);

    ApiKeyConfig kc;
    kc.key_hash = hash;
    kc.tenant_id = "tenant_ns";
    kc.permissions = kPermRead;
    kc.allowed_namespaces = {"ns1", "ns2"};

    auth_.LoadKeys({kc});

    AuthContext ctx;
    Status s = auth_.Authenticate(key, &ctx);
    EXPECT_TRUE(s.ok());
    EXPECT_EQ(ctx.namespaces.size(), 2u);
    EXPECT_EQ(ctx.namespaces[0], "ns1");
    EXPECT_EQ(ctx.namespaces[1], "ns2");
}

TEST_F(ApiKeyAuthTest, AuthorizeReadPermission) {
    AuthContext ctx;
    ctx.permissions = kPermRead;
    Status s = auth_.Authorize(ctx, "", kPermRead);
    EXPECT_TRUE(s.ok());
}

TEST_F(ApiKeyAuthTest, AuthorizeInsufficientPermission) {
    AuthContext ctx;
    ctx.permissions = kPermRead;
    Status s = auth_.Authorize(ctx, "", kPermAdmin);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kPermissionDenied);
    EXPECT_EQ(s.message(), "ADMIN permission required");
}

TEST_F(ApiKeyAuthTest, AuthorizeWriteInsufficient) {
    AuthContext ctx;
    ctx.permissions = kPermRead;
    Status s = auth_.Authorize(ctx, "", kPermWrite);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kPermissionDenied);
    EXPECT_EQ(s.message(), "WRITE permission required");
}

TEST_F(ApiKeyAuthTest, AuthorizeNamespaceAllowed) {
    AuthContext ctx;
    ctx.permissions = kPermRead;
    ctx.namespaces = {"ns1", "ns2"};
    Status s = auth_.Authorize(ctx, "ns1", kPermRead);
    EXPECT_TRUE(s.ok());
}

TEST_F(ApiKeyAuthTest, AuthorizeNamespaceDenied) {
    AuthContext ctx;
    ctx.permissions = kPermRead;
    ctx.namespaces = {"ns1", "ns2"};
    Status s = auth_.Authorize(ctx, "ns3", kPermRead);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kPermissionDenied);
    EXPECT_EQ(s.message(), "Access denied for namespace 'ns3'");
}

TEST_F(ApiKeyAuthTest, AuthorizeEmptyNamespaceList) {
    AuthContext ctx;
    ctx.permissions = kPermRead;
    ctx.namespaces = {};  // empty = all allowed
    Status s = auth_.Authorize(ctx, "any-ns", kPermRead);
    EXPECT_TRUE(s.ok());
}

TEST_F(ApiKeyAuthTest, AuthorizeEmptyNamespaceParam) {
    AuthContext ctx;
    ctx.permissions = kPermRead;
    ctx.namespaces = {"ns1"};
    // Empty namespace param should skip namespace check
    Status s = auth_.Authorize(ctx, "", kPermRead);
    EXPECT_TRUE(s.ok());
}

// --- Additional edge-case tests ---

TEST_F(ApiKeyAuthTest, AuthenticateFutureExpiry) {
    // Key that expires far in the future should authenticate successfully
    std::string future_key = "future-key-789";
    std::string future_hash = ApiKeyAuth::HashKey(future_key);

    ApiKeyConfig kc;
    kc.key_hash = future_hash;
    kc.tenant_id = "tenant_future";
    kc.permissions = kPermRead;
    kc.expires_at = 4102444800;  // 2100-01-01 UTC

    auth_.LoadKeys({kc});

    AuthContext ctx;
    Status s = auth_.Authenticate(future_key, &ctx);
    EXPECT_TRUE(s.ok());
    EXPECT_EQ(ctx.tenant_id, "tenant_future");
}

TEST_F(ApiKeyAuthTest, AuthorizeCombinedPermissions) {
    // User with READ | WRITE should pass when READ is required
    AuthContext ctx;
    ctx.permissions = kPermRead | kPermWrite;
    EXPECT_TRUE(auth_.Authorize(ctx, "", kPermRead).ok());
    EXPECT_TRUE(auth_.Authorize(ctx, "", kPermWrite).ok());
    // But not ADMIN
    EXPECT_FALSE(auth_.Authorize(ctx, "", kPermAdmin).ok());
}

TEST_F(ApiKeyAuthTest, AuthorizeReadWriteAdminFull) {
    // Full permission set should pass all checks
    AuthContext ctx;
    ctx.permissions = kPermRead | kPermWrite | kPermAdmin;
    EXPECT_TRUE(auth_.Authorize(ctx, "", kPermRead).ok());
    EXPECT_TRUE(auth_.Authorize(ctx, "", kPermWrite).ok());
    EXPECT_TRUE(auth_.Authorize(ctx, "", kPermAdmin).ok());
    // Combined permission check
    EXPECT_TRUE(auth_.Authorize(ctx, "", kPermRead | kPermWrite).ok());
}

TEST_F(ApiKeyAuthTest, AuthorizeZeroPermissions) {
    // Zero permission bitmask should fail all checks
    AuthContext ctx;
    ctx.permissions = 0;
    EXPECT_FALSE(auth_.Authorize(ctx, "", kPermRead).ok());
    EXPECT_FALSE(auth_.Authorize(ctx, "", kPermWrite).ok());
    EXPECT_FALSE(auth_.Authorize(ctx, "", kPermAdmin).ok());
}

TEST_F(ApiKeyAuthTest, LoadKeysReplacesExisting) {
    // Loading new keys should replace the previous key set
    AuthContext ctx;

    // First, original key should work
    EXPECT_TRUE(auth_.Authenticate(test_key_, &ctx).ok());

    // Now load a different key set that does NOT include test_key_
    std::string new_key = "brand-new-key";
    std::string new_hash = ApiKeyAuth::HashKey(new_key);
    ApiKeyConfig kc;
    kc.key_hash = new_hash;
    kc.tenant_id = "tenant_new";
    kc.permissions = kPermRead;
    kc.expires_at = 0;
    auth_.LoadKeys({kc});

    // Original key should now be rejected
    AuthContext ctx2;
    EXPECT_FALSE(auth_.Authenticate(test_key_, &ctx2).ok());

    // New key should work
    AuthContext ctx3;
    EXPECT_TRUE(auth_.Authenticate(new_key, &ctx3).ok());
    EXPECT_EQ(ctx3.tenant_id, "tenant_new");
}

TEST_F(ApiKeyAuthTest, HashKeyEmptyInput) {
    // Hashing an empty string should return a valid 64-char hex digest
    std::string hash = ApiKeyAuth::HashKey("");
    EXPECT_EQ(hash.size(), 64u);
    // SHA-256 of empty string is a well-known constant
    EXPECT_EQ(hash, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST_F(ApiKeyAuthTest, AuthContextCanAccessNamespaceWildcard) {
    // Empty namespace list = wildcard access to all namespaces
    AuthContext ctx;
    ctx.namespaces = {};
    EXPECT_TRUE(ctx.can_access_namespace("anything"));
    EXPECT_TRUE(ctx.can_access_namespace("ns1"));
}

TEST_F(ApiKeyAuthTest, AuthContextPermissionHelpers) {
    AuthContext ctx;
    ctx.permissions = kPermRead | kPermWrite;
    EXPECT_TRUE(ctx.can_read());
    EXPECT_TRUE(ctx.can_write());
    EXPECT_FALSE(ctx.is_admin());

    ctx.permissions = kPermAdmin;
    EXPECT_FALSE(ctx.can_read());
    EXPECT_FALSE(ctx.can_write());
    EXPECT_TRUE(ctx.is_admin());
}

TEST_F(ApiKeyAuthTest, AuthorizeNamespaceCaseSensitive) {
    // Namespace matching should be case-sensitive
    AuthContext ctx;
    ctx.permissions = kPermRead;
    ctx.namespaces = {"MyNs"};
    EXPECT_TRUE(auth_.Authorize(ctx, "MyNs", kPermRead).ok());
    EXPECT_FALSE(auth_.Authorize(ctx, "myns", kPermRead).ok());
    EXPECT_FALSE(auth_.Authorize(ctx, "MYNS", kPermRead).ok());
}

TEST_F(ApiKeyAuthTest, LoadKeysEmpty) {
    // Loading an empty key set should remove all keys
    auth_.LoadKeys({});

    AuthContext ctx;
    Status s = auth_.Authenticate(test_key_, &ctx);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kUnauthenticated);
}

TEST_F(ApiKeyAuthTest, AuthenticateWhitespaceKey) {
    // Whitespace-only key should be treated as invalid (not matching any stored key)
    AuthContext ctx;
    Status s = auth_.Authenticate("   ", &ctx);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kUnauthenticated);
}

TEST_F(ApiKeyAuthTest, LoadKeysMultipleKeys) {
    // Load multiple keys and ensure each resolves to the correct tenant
    std::string key_a = "key-alpha";
    std::string key_b = "key-beta";

    ApiKeyConfig kc_a;
    kc_a.key_hash = ApiKeyAuth::HashKey(key_a);
    kc_a.tenant_id = "tenant_alpha";
    kc_a.permissions = kPermRead;
    kc_a.expires_at = 0;

    ApiKeyConfig kc_b;
    kc_b.key_hash = ApiKeyAuth::HashKey(key_b);
    kc_b.tenant_id = "tenant_beta";
    kc_b.permissions = kPermRead | kPermWrite;
    kc_b.expires_at = 0;

    auth_.LoadKeys({kc_a, kc_b});

    AuthContext ctx_a;
    EXPECT_TRUE(auth_.Authenticate(key_a, &ctx_a).ok());
    EXPECT_EQ(ctx_a.tenant_id, "tenant_alpha");
    EXPECT_EQ(ctx_a.permissions, kPermRead);

    AuthContext ctx_b;
    EXPECT_TRUE(auth_.Authenticate(key_b, &ctx_b).ok());
    EXPECT_EQ(ctx_b.tenant_id, "tenant_beta");
    EXPECT_EQ(ctx_b.permissions, kPermRead | kPermWrite);
}

TEST_F(ApiKeyAuthTest, AuthorizeMultipleNamespaces) {
    // Verify access against a longer namespace list
    AuthContext ctx;
    ctx.permissions = kPermRead;
    ctx.namespaces = {"ns1", "ns2", "ns3", "ns4", "ns5"};

    EXPECT_TRUE(auth_.Authorize(ctx, "ns3", kPermRead).ok());
    EXPECT_TRUE(auth_.Authorize(ctx, "ns5", kPermRead).ok());
    EXPECT_FALSE(auth_.Authorize(ctx, "ns6", kPermRead).ok());
}

TEST_F(ApiKeyAuthTest, AuthContextUserIdMatchesTenantId) {
    // MVP: user_id should be set equal to tenant_id after authentication
    AuthContext ctx;
    Status s = auth_.Authenticate(test_key_, &ctx);
    EXPECT_TRUE(s.ok());
    EXPECT_EQ(ctx.user_id, ctx.tenant_id);
    EXPECT_EQ(ctx.user_id, "tenant_001");
}

TEST_F(ApiKeyAuthTest, AuthContextAgentIdEmpty) {
    // Agent ID should be empty after basic API key authentication
    AuthContext ctx;
    Status s = auth_.Authenticate(test_key_, &ctx);
    EXPECT_TRUE(s.ok());
    EXPECT_TRUE(ctx.agent_id.empty());
}

// --- Design alignment: verify error code strings match API_GATEWAY_DESIGN ---

TEST_F(ApiKeyAuthTest, InvalidKeyErrorCode) {
    // Per API_GATEWAY_DESIGN.md section 2.5.2: invalid key -> INVALID_API_KEY
    AuthContext ctx;
    Status s = auth_.Authenticate("wrong-key", &ctx);
    EXPECT_EQ(s.error_code_string(), "INVALID_API_KEY");
}

TEST_F(ApiKeyAuthTest, ExpiredKeyErrorCode) {
    // Per API_GATEWAY_DESIGN.md section 2.5.2: expired key -> EXPIRED_API_KEY
    std::string expired_key = "expired-for-code-test";
    ApiKeyConfig kc;
    kc.key_hash = ApiKeyAuth::HashKey(expired_key);
    kc.tenant_id = "t";
    kc.permissions = 7;
    kc.expires_at = 1;
    auth_.LoadKeys({kc});

    AuthContext ctx;
    Status s = auth_.Authenticate(expired_key, &ctx);
    EXPECT_EQ(s.error_code_string(), "EXPIRED_API_KEY");
}

TEST_F(ApiKeyAuthTest, EmptyKeyErrorCode) {
    // Per API_GATEWAY_DESIGN.md section 2.5.2: missing auth -> UNAUTHORIZED
    AuthContext ctx;
    Status s = auth_.Authenticate("", &ctx);
    EXPECT_EQ(s.error_code_string(), "UNAUTHORIZED");
}

TEST_F(ApiKeyAuthTest, NamespaceDeniedErrorCode) {
    // Per API_GATEWAY_DESIGN.md section 2.5.2: namespace denied -> NAMESPACE_DENIED
    AuthContext ctx;
    ctx.permissions = kPermRead;
    ctx.namespaces = {"ns1"};
    Status s = auth_.Authorize(ctx, "ns99", kPermRead);
    EXPECT_EQ(s.error_code_string(), "NAMESPACE_DENIED");
}

TEST_F(ApiKeyAuthTest, PermissionDeniedErrorCode) {
    // Per API_GATEWAY_DESIGN.md section 2.5.2: permission denied -> FORBIDDEN
    AuthContext ctx;
    ctx.permissions = kPermRead;
    Status s = auth_.Authorize(ctx, "", kPermWrite);
    EXPECT_EQ(s.error_code_string(), "FORBIDDEN");
}

TEST_F(ApiKeyAuthTest, HashKeyLongInput) {
    // Hashing a long input should still produce a 64-char hex digest
    std::string long_input(10000, 'a');
    std::string hash = ApiKeyAuth::HashKey(long_input);
    EXPECT_EQ(hash.size(), 64u);
}

TEST_F(ApiKeyAuthTest, HashKeyBinaryInput) {
    // Hashing input with null bytes should work correctly
    std::string binary_input = std::string("key\0with\0nulls", 14);
    std::string hash = ApiKeyAuth::HashKey(binary_input);
    EXPECT_EQ(hash.size(), 64u);
    // Hash should be consistent
    EXPECT_EQ(hash, ApiKeyAuth::HashKey(binary_input));
}

}  // namespace
}  // namespace cortrix
