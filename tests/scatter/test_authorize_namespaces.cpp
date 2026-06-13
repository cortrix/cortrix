#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "cortrix/auth/auth_context.h"
#include "cortrix/query/authorize_namespaces.h"
#include "cortrix/query/cross_ns_error.h"
#include "mock_permission_service.h"

// S2.1 coverage: AuthorizeNamespaces 5-step (§4.2) + ExpandNamespaces (["*"] +
// hard cap 100) + anti-enumeration (NS-not-found == unauthorized).
namespace cortrix::query {
namespace {

AuthContext MakeAuth(const std::string& user_id = "u1") {
    AuthContext a;
    a.user_id = user_id;
    a.tenant_id = "t1";
    return a;
}

// Step 1: empty user_id → CX_ERR_AUTH_INVALID_CREDENTIALS.
TEST(AuthorizeNamespacesTest, UnauthenticatedThrowsAuthError) {
    MockPermissionService perm({"ns_a"});
    AuthContext anon;  // user_id empty
    try {
        AuthorizeNamespaces({"ns_a"}, anon, &perm);
        FAIL() << "expected CrossNsException";
    } catch (const CrossNsException& e) {
        EXPECT_EQ(e.code(), CrossNsErrorCode::kAuthInvalidCredentials);
    }
}

// Step 3-5: all authorized → returns the (de-duplicated, ordered) list.
TEST(AuthorizeNamespacesTest, AllAuthorizedReturnsList) {
    MockPermissionService perm({"ns_a", "ns_b", "ns_c"});
    auto out = AuthorizeNamespaces({"ns_a", "ns_b"}, MakeAuth(), &perm);
    EXPECT_EQ(out, (std::vector<std::string>{"ns_a", "ns_b"}));
}

// Step 4: any unauthorized → CX_ERR_NS_UNAUTHORIZED + structured unauthorized list.
TEST(AuthorizeNamespacesTest, UnauthorizedThrowsWithStructuredList) {
    MockPermissionService perm({"ns_a"});  // ns_b NOT authorized
    try {
        AuthorizeNamespaces({"ns_a", "ns_b"}, MakeAuth(), &perm);
        FAIL() << "expected CrossNsException";
    } catch (const CrossNsException& e) {
        EXPECT_EQ(e.code(), CrossNsErrorCode::kNsUnauthorized);
        ASSERT_TRUE(e.GetError().structured_data.has_value());
        auto list = (*e.GetError().structured_data)["unauthorized_namespaces"];
        ASSERT_TRUE(list.is_array());
        EXPECT_EQ(list.size(), 1u);
        EXPECT_EQ(list[0], "ns_b");
    }
}

// anti-enumeration (Issue 2.6): a non-existent NS is reported as unauthorized, not a distinct
// "not found" — existence is never leaked.
TEST(AuthorizeNamespacesTest, NonexistentNsTreatedAsUnauthorized) {
    MockPermissionService perm({"ns_a"});  // "ghost" was never created
    try {
        AuthorizeNamespaces({"ghost"}, MakeAuth(), &perm);
        FAIL() << "expected CrossNsException";
    } catch (const CrossNsException& e) {
        EXPECT_EQ(e.code(), CrossNsErrorCode::kNsUnauthorized);
        auto list = (*e.GetError().structured_data)["unauthorized_namespaces"];
        EXPECT_EQ(list[0], "ghost");
    }
}

// ExpandNamespaces: ["*"] → the principal's authorized set.
TEST(AuthorizeNamespacesTest, WildcardExpandsToAuthorizedSet) {
    MockPermissionService perm({"ns_a", "ns_b", "ns_c"});
    auto out = AuthorizeNamespaces({"*"}, MakeAuth(), &perm);
    EXPECT_EQ(out, (std::vector<std::string>{"ns_a", "ns_b", "ns_c"}));
}

// ExpandNamespaces hard cap (Issue 1.5): > max → CX_ERR_TOO_MANY_NAMESPACES with
// {requested_count, max_namespaces}.
TEST(AuthorizeNamespacesTest, OverCapThrowsTooManyNamespaces) {
    MockPermissionService perm;
    std::vector<std::string> many;
    for (int i = 0; i < 5; ++i) many.push_back("ns_" + std::to_string(i));
    try {
        // cap = 3 < 5 requested
        AuthorizeNamespaces(many, MakeAuth(), &perm, /*max_namespaces=*/3);
        FAIL() << "expected CrossNsException";
    } catch (const CrossNsException& e) {
        EXPECT_EQ(e.code(), CrossNsErrorCode::kTooManyNamespaces);
        ASSERT_TRUE(e.GetError().structured_data.has_value());
        EXPECT_EQ((*e.GetError().structured_data)["requested_count"], 5);
        EXPECT_EQ((*e.GetError().structured_data)["max_namespaces"], 3);
    }
}

// Cap is enforced AFTER wildcard expansion (a principal authorized for > cap NS).
TEST(AuthorizeNamespacesTest, WildcardExpansionAlsoHitsCap) {
    std::vector<std::string> auth_set;
    for (int i = 0; i < 10; ++i) auth_set.push_back("ns_" + std::to_string(i));
    MockPermissionService perm(auth_set);
    try {
        AuthorizeNamespaces({"*"}, MakeAuth(), &perm, /*max_namespaces=*/5);
        FAIL() << "expected CrossNsException";
    } catch (const CrossNsException& e) {
        EXPECT_EQ(e.code(), CrossNsErrorCode::kTooManyNamespaces);
        EXPECT_EQ((*e.GetError().structured_data)["requested_count"], 10);
    }
}

// Exactly at the cap is allowed (boundary).
TEST(AuthorizeNamespacesTest, ExactlyAtCapAllowed) {
    MockPermissionService perm({"ns_0", "ns_1", "ns_2"});
    auto out = AuthorizeNamespaces({"ns_0", "ns_1", "ns_2"}, MakeAuth(), &perm,
                                   /*max_namespaces=*/3);
    EXPECT_EQ(out.size(), 3u);
}

// ExpandNamespaces de-duplicates while preserving first-seen order.
TEST(AuthorizeNamespacesTest, ExpandDedupsPreservingOrder) {
    MockPermissionService perm({"ns_a", "ns_b"});
    auto out = ExpandNamespaces({"ns_b", "ns_a", "ns_b"}, MakeAuth(), &perm);
    EXPECT_EQ(out, (std::vector<std::string>{"ns_b", "ns_a"}));
}

// Default cap is the spec value 100.
TEST(AuthorizeNamespacesTest, DefaultCapIs100) {
    EXPECT_EQ(kDefaultMaxNamespaces, 100);
}

}  // namespace
}  // namespace cortrix::query
