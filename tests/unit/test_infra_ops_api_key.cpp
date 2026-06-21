#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "cortrix/auth/api_key_auth.h"

// BREADTH op-matrix coverage for ApiKeyAuth (API gateway). Complements
// test_api_key_auth.cpp with: HashKey determinism/length matrix, LoadKeys +
// Authenticate over (permission, expiry, tenant) matrices, and Authorize over a
// (held-permissions x required-permission) matrix plus the namespace-seam matrix.
//
// ApiKeyAuth is NON-COPYABLE/-movable (mutex member) -> built in place / by ref.
// Suite names are globally unique: ApiKeyOpsMatrix* (no clash with ApiKeyAuthTest).
namespace cortrix {
namespace {

// A fixed "far future" expiry (year ~2100) used as the "never-expires-in-practice"
// sentinel for matrix rows that should authenticate.
constexpr int64_t kFarFuture = 4102444800;  // 2100-01-01 UTC

// -- HashKey: 64-char hex digest + determinism over an input matrix ----------
class ApiKeyOpsMatrixHash : public ::testing::TestWithParam<std::string> {};

TEST_P(ApiKeyOpsMatrixHash, DigestIs64HexAndDeterministic) {
    const std::string& in = GetParam();
    std::string h1 = ApiKeyAuth::HashKey(in);
    std::string h2 = ApiKeyAuth::HashKey(in);
    EXPECT_EQ(h1.size(), 64u);
    EXPECT_EQ(h1, h2);
    for (char c : h1) {
        EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))
            << "non-lowercase-hex char in digest: " << c;
    }
}

INSTANTIATE_TEST_SUITE_P(
    Inputs, ApiKeyOpsMatrixHash,
    ::testing::Values(std::string(""), std::string("a"), std::string("b"),
                      std::string("ab"), std::string("test"), std::string("Test"),
                      std::string("key-1"), std::string("KEY-1"),
                      std::string("key_2"), std::string("key.3"),
                      std::string("  leading"), std::string("trailing  "),
                      std::string("   "), std::string(10, 'z'),
                      std::string(100, 'q'), std::string(1000, 'x'),
                      std::string(10000, 'a'),
                      std::string(std::string("k\0z\0q", 5)),
                      std::string(std::string("\0\0\0", 3)),
                      std::string("unicode-\xC3\xA9\xC3\xA8"),
                      std::string("sk-live-1234567890abcdef"),
                      std::string("tab\tnewline\n")));

// -- HashKey: distinct inputs -> distinct digests (collision-free matrix) -----
TEST(ApiKeyOpsMatrixHashDistinct, DifferentInputsDifferentDigests) {
    std::vector<std::string> inputs = {"k0", "k1", "k2", "k3", "k4",
                                       "k5", "k6", "k7", "k8", "k9"};
    std::vector<std::string> digests;
    for (const auto& in : inputs) digests.push_back(ApiKeyAuth::HashKey(in));
    for (size_t i = 0; i < digests.size(); ++i)
        for (size_t j = i + 1; j < digests.size(); ++j)
            EXPECT_NE(digests[i], digests[j]) << "collision " << i << "," << j;
}

// -- Authenticate over a permission matrix -----------------------------------
class ApiKeyOpsMatrixAuthPerms : public ::testing::TestWithParam<int> {};

TEST_P(ApiKeyOpsMatrixAuthPerms, PermissionsRoundTripThroughAuthenticate) {
    const int perms = GetParam();
    ApiKeyAuth auth;  // built in place, used by reference
    const std::string plain = "perm-key-" + std::to_string(perms);
    ApiKeyConfig kc;
    kc.key_hash = ApiKeyAuth::HashKey(plain);
    kc.tenant_id = "tenant-p";
    kc.permissions = perms;
    kc.expires_at = 0;  // never expires
    auth.LoadKeys({kc});

    AuthContext ctx;
    Status s = auth.Authenticate(plain, &ctx);
    EXPECT_TRUE(s.ok());
    EXPECT_EQ(ctx.permissions, perms);
    EXPECT_EQ(ctx.tenant_id, "tenant-p");
    EXPECT_EQ(ctx.user_id, "tenant-p");   // MVP: user_id == tenant_id
    EXPECT_TRUE(ctx.agent_id.empty());
}

INSTANTIATE_TEST_SUITE_P(
    Perms, ApiKeyOpsMatrixAuthPerms,
    ::testing::Values(0, kPermRead, kPermWrite, kPermAdmin,
                      kPermRead | kPermWrite, kPermRead | kPermAdmin,
                      kPermWrite | kPermAdmin,
                      kPermRead | kPermWrite | kPermAdmin));

// -- Authenticate over an expiry matrix --------------------------------------
struct ExpiryCase { int64_t expires_at; bool expect_ok; };
class ApiKeyOpsMatrixExpiry : public ::testing::TestWithParam<ExpiryCase> {};

TEST_P(ApiKeyOpsMatrixExpiry, ExpiryGatesAuthentication) {
    const auto p = GetParam();
    ApiKeyAuth auth;
    const std::string plain = "exp-key";
    ApiKeyConfig kc;
    kc.key_hash = ApiKeyAuth::HashKey(plain);
    kc.tenant_id = "t-exp";
    kc.permissions = kPermRead;
    kc.expires_at = p.expires_at;
    auth.LoadKeys({kc});

    AuthContext ctx;
    Status s = auth.Authenticate(plain, &ctx);
    EXPECT_EQ(s.ok(), p.expect_ok) << "expires_at=" << p.expires_at;
    if (!p.expect_ok) {
        EXPECT_EQ(s.code(), StatusCode::kUnauthenticated);
        EXPECT_EQ(s.message(), "API key expired");
        EXPECT_EQ(s.error_code_string(), "EXPIRED_API_KEY");
    }
}

INSTANTIATE_TEST_SUITE_P(
    Expiries, ApiKeyOpsMatrixExpiry,
    ::testing::Values(ExpiryCase{0, true},        // 0 = never expires
                      ExpiryCase{1, false},       // 1970 -> expired
                      ExpiryCase{2, false},
                      ExpiryCase{1000, false},
                      ExpiryCase{100000, false},
                      ExpiryCase{1000000000, false},  // 2001 -> expired
                      ExpiryCase{1700000000, false},  // 2023 -> expired
                      ExpiryCase{kFarFuture, true},
                      ExpiryCase{kFarFuture + 1000, true},
                      ExpiryCase{9000000000, true}));  // ~2255 -> valid

// -- Authenticate: empty / unknown key matrix --------------------------------
struct BadKeyCase { std::string key; const char* code; std::string msg; };
class ApiKeyOpsMatrixBadKey : public ::testing::TestWithParam<BadKeyCase> {};

TEST_P(ApiKeyOpsMatrixBadKey, RejectsWithExpectedCode) {
    const auto p = GetParam();
    ApiKeyAuth auth;
    ApiKeyConfig kc;
    kc.key_hash = ApiKeyAuth::HashKey("the-only-valid-key");
    kc.tenant_id = "t";
    kc.permissions = kPermRead;
    kc.expires_at = 0;
    auth.LoadKeys({kc});

    AuthContext ctx;
    Status s = auth.Authenticate(p.key, &ctx);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kUnauthenticated);
    EXPECT_EQ(s.error_code_string(), p.code);
    if (!p.msg.empty()) EXPECT_EQ(s.message(), p.msg);
}

INSTANTIATE_TEST_SUITE_P(
    BadKeys, ApiKeyOpsMatrixBadKey,
    ::testing::Values(
        BadKeyCase{"", "UNAUTHORIZED", "API key is empty"},
        BadKeyCase{"wrong", "INVALID_API_KEY", "Invalid API key"},
        BadKeyCase{"   ", "INVALID_API_KEY", "Invalid API key"},
        BadKeyCase{"\t", "INVALID_API_KEY", "Invalid API key"},
        BadKeyCase{"the-only-valid-key ", "INVALID_API_KEY", "Invalid API key"},
        BadKeyCase{" the-only-valid-key", "INVALID_API_KEY", "Invalid API key"},
        BadKeyCase{"the-only-valid-key-extra", "INVALID_API_KEY", "Invalid API key"},
        BadKeyCase{"THE-ONLY-VALID-KEY", "INVALID_API_KEY", "Invalid API key"},
        BadKeyCase{"the_only_valid_key", "INVALID_API_KEY", "Invalid API key"},
        BadKeyCase{"random-garbage-99", "INVALID_API_KEY", "Invalid API key"}));

// -- LoadKeys multi-key resolution matrix ------------------------------------
class ApiKeyOpsMatrixMultiKey : public ::testing::TestWithParam<int> {};

TEST_P(ApiKeyOpsMatrixMultiKey, EachKeyResolvesToItsTenant) {
    const int count = GetParam();
    ApiKeyAuth auth;
    std::vector<ApiKeyConfig> cfgs;
    for (int i = 0; i < count; ++i) {
        ApiKeyConfig kc;
        kc.key_hash = ApiKeyAuth::HashKey("mk-" + std::to_string(i));
        kc.tenant_id = "tenant-" + std::to_string(i);
        kc.permissions = (i % 2 == 0) ? kPermRead : (kPermRead | kPermWrite);
        kc.expires_at = 0;
        cfgs.push_back(kc);
    }
    auth.LoadKeys(cfgs);
    for (int i = 0; i < count; ++i) {
        AuthContext ctx;
        Status s = auth.Authenticate("mk-" + std::to_string(i), &ctx);
        ASSERT_TRUE(s.ok()) << "key " << i;
        EXPECT_EQ(ctx.tenant_id, "tenant-" + std::to_string(i));
    }
}

INSTANTIATE_TEST_SUITE_P(Counts, ApiKeyOpsMatrixMultiKey,
                         ::testing::Values(1, 2, 3, 4, 5, 8, 10, 15, 20, 25, 50,
                                           100));

// -- LoadKeys replacement matrix: each LoadKeys fully replaces the prior set --
TEST(ApiKeyOpsMatrixReplace, LoadKeysReplacesPriorSetAcrossRounds) {
    ApiKeyAuth auth;
    for (int round = 0; round < 5; ++round) {
        ApiKeyConfig kc;
        kc.key_hash = ApiKeyAuth::HashKey("round-" + std::to_string(round));
        kc.tenant_id = "t";
        kc.permissions = kPermRead;
        kc.expires_at = 0;
        auth.LoadKeys({kc});

        AuthContext ctx;
        EXPECT_TRUE(auth.Authenticate("round-" + std::to_string(round), &ctx).ok());
        if (round > 0) {
            AuthContext old;
            EXPECT_FALSE(
                auth.Authenticate("round-" + std::to_string(round - 1), &old).ok());
        }
    }
    // Empty load clears everything.
    auth.LoadKeys({});
    AuthContext ctx;
    EXPECT_FALSE(auth.Authenticate("round-4", &ctx).ok());
}

// -- Authorize: held x required permission matrix ----------------------------
struct AuthzCase { int held; int required; bool expect_ok; const char* msg; };
class ApiKeyOpsMatrixAuthorize : public ::testing::TestWithParam<AuthzCase> {};

TEST_P(ApiKeyOpsMatrixAuthorize, PermissionGate) {
    const auto p = GetParam();
    ApiKeyAuth auth;
    AuthContext ctx;
    ctx.permissions = p.held;
    Status s = auth.Authorize(ctx, "", p.required);  // empty ns -> only perm gate
    EXPECT_EQ(s.ok(), p.expect_ok)
        << "held=" << p.held << " required=" << p.required;
    if (!p.expect_ok) {
        EXPECT_EQ(s.code(), StatusCode::kPermissionDenied);
        EXPECT_EQ(s.error_code_string(), "FORBIDDEN");
        if (p.msg && *p.msg) EXPECT_EQ(s.message(), p.msg);
    }
}

INSTANTIATE_TEST_SUITE_P(
    Gate, ApiKeyOpsMatrixAuthorize,
    ::testing::Values(
        AuthzCase{kPermRead, kPermRead, true, ""},
        AuthzCase{kPermRead, kPermWrite, false, "WRITE permission required"},
        AuthzCase{kPermRead, kPermAdmin, false, "ADMIN permission required"},
        AuthzCase{kPermWrite, kPermWrite, true, ""},
        AuthzCase{kPermWrite, kPermRead, false, ""},
        AuthzCase{kPermAdmin, kPermAdmin, true, ""},
        AuthzCase{kPermAdmin, kPermRead, false, ""},
        AuthzCase{kPermRead | kPermWrite, kPermRead, true, ""},
        AuthzCase{kPermRead | kPermWrite, kPermWrite, true, ""},
        AuthzCase{kPermRead | kPermWrite, kPermAdmin, false,
                  "ADMIN permission required"},
        AuthzCase{kPermRead | kPermWrite | kPermAdmin, kPermRead, true, ""},
        AuthzCase{kPermRead | kPermWrite | kPermAdmin, kPermWrite, true, ""},
        AuthzCase{kPermRead | kPermWrite | kPermAdmin, kPermAdmin, true, ""},
        AuthzCase{kPermRead | kPermWrite | kPermAdmin,
                  kPermRead | kPermWrite, true, ""},
        AuthzCase{0, kPermRead, false, ""},
        AuthzCase{0, kPermWrite, false, "WRITE permission required"},
        AuthzCase{0, kPermAdmin, false, "ADMIN permission required"}));

// -- Authorize namespace-seam matrix: allow-set decides, empty ns skips gate --
struct NsSeamCase { std::string ns; bool expect_ok; };
class ApiKeyOpsMatrixNsSeam : public ::testing::TestWithParam<NsSeamCase> {};

TEST_P(ApiKeyOpsMatrixNsSeam, SeamGrantsOnlyTheAllowSet) {
    const auto p = GetParam();
    ApiKeyAuth auth;
    auth.SetNamespaceAuthorizer([](const AuthContext&, const std::string& ns) {
        return ns == "ns1" || ns == "ns2" || ns == "ns3";
    });
    AuthContext ctx;
    ctx.permissions = kPermRead;
    Status s = auth.Authorize(ctx, p.ns, kPermRead);
    EXPECT_EQ(s.ok(), p.expect_ok) << "ns=" << p.ns;
    if (!p.expect_ok) {
        EXPECT_EQ(s.code(), StatusCode::kPermissionDenied);
        // Anti-enumeration: canonical identity, never echoes the ns name.
        EXPECT_EQ(s.message(), "CX_ERR_NS_UNAUTHORIZED");
    }
}

INSTANTIATE_TEST_SUITE_P(
    NsSeam, ApiKeyOpsMatrixNsSeam,
    ::testing::Values(NsSeamCase{"", true},      // empty ns -> gate skipped
                      NsSeamCase{"ns1", true}, NsSeamCase{"ns2", true},
                      NsSeamCase{"ns3", true}, NsSeamCase{"ns4", false},
                      NsSeamCase{"ns0", false}, NsSeamCase{"NS1", false},
                      NsSeamCase{"Ns2", false}, NsSeamCase{"ns1 ", false},
                      NsSeamCase{" ns1", false}, NsSeamCase{"ns11", false},
                      NsSeamCase{"ns99", false}, NsSeamCase{"other", false}));

// -- Authorize: no seam installed leaves any namespace ungated ---------------
class ApiKeyOpsMatrixNoSeam : public ::testing::TestWithParam<std::string> {};

TEST_P(ApiKeyOpsMatrixNoSeam, UngatedWithoutAuthorizer) {
    ApiKeyAuth auth;  // no authorizer installed
    AuthContext ctx;
    ctx.permissions = kPermRead;
    EXPECT_TRUE(auth.Authorize(ctx, GetParam(), kPermRead).ok());
}

INSTANTIATE_TEST_SUITE_P(Nss, ApiKeyOpsMatrixNoSeam,
                         ::testing::Values(std::string(""), std::string("any"),
                                           std::string("ns-x"),
                                           std::string("anything-goes")));

// -- enabled() toggle matrix (state accessor) --------------------------------
TEST(ApiKeyOpsMatrixEnabled, ToggleReflectsState) {
    ApiKeyAuth auth;
    EXPECT_TRUE(auth.enabled());  // default
    auth.set_enabled(false);
    EXPECT_FALSE(auth.enabled());
    auth.set_enabled(true);
    EXPECT_TRUE(auth.enabled());
}

// -- ListAuthorizedNamespaces seam matrix ------------------------------------
TEST(ApiKeyOpsMatrixNsLister, ListerSeamReturnsAllowSetOrEmpty) {
    ApiKeyAuth auth;
    EXPECT_FALSE(auth.has_namespace_lister());
    AuthContext ctx;
    EXPECT_TRUE(auth.ListAuthorizedNamespaces(ctx).empty());  // unset -> empty

    auth.SetNamespaceLister(
        [](const AuthContext&) { return std::vector<std::string>{"a", "b", "c"}; });
    EXPECT_TRUE(auth.has_namespace_lister());
    auto v = auth.ListAuthorizedNamespaces(ctx);
    ASSERT_EQ(v.size(), 3u);
    EXPECT_EQ(v[0], "a");
    EXPECT_EQ(v[2], "c");
}

}  // namespace
}  // namespace cortrix
