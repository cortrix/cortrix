// Tenancy -- tenant error registry contract tests (S1).
// Mirrors test_auth_error.cpp / the catalog-error contract: the enum, its
// CX_ERR_* strings, categories, retryability, and structured_data key sets are a
// stable Agent-friendly contract (GEN-Agent #1/#4/#5/#7) that must not drift.
#include <gtest/gtest.h>

#include <optional>
#include <regex>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cortrix/agent_friendly/error.h"
#include "cortrix/tenant/tenant_error.h"

using namespace cortrix::tenant;
using cortrix::agent_friendly::ErrorCategory;

namespace {

// The full enum, enumerated once so every test iterates the same closed set.
const std::vector<TenantErrorCode>& AllCodes() {
    static const std::vector<TenantErrorCode> kAll = {
        TenantErrorCode::kTenantNotFound,
        TenantErrorCode::kTenantAlreadyExists,
        TenantErrorCode::kTenantEmailDuplicate,
        TenantErrorCode::kTenantNameDuplicate,
        TenantErrorCode::kTenantDeleteHasNamespaces,
        TenantErrorCode::kTenantDeleteHasMembers,
        TenantErrorCode::kTenantTransferTargetNotMember,
        TenantErrorCode::kTenantLastOwnerRemoval,
        TenantErrorCode::kMemberAlreadyExists,
        TenantErrorCode::kMemberNotFound,
        TenantErrorCode::kMemberRoleInvalid,
        TenantErrorCode::kNsUnauthorized,
        TenantErrorCode::kNsNotFound,
        TenantErrorCode::kAclGrantToOwnerTenant,
        TenantErrorCode::kAclDuplicateGrant,
        TenantErrorCode::kAclGrantInvalidRole,
        TenantErrorCode::kAclNotFound,
        TenantErrorCode::kPermissionOwnerOnly,
        TenantErrorCode::kTenantQuotaExceeded,
        TenantErrorCode::kTenantQuotaNotFound,
        TenantErrorCode::kTenantTransactionFailed,
        TenantErrorCode::kAdminKeyRequired,
        TenantErrorCode::kCallerNotMember,
        TenantErrorCode::kCallerRoleInsufficient,
    };
    return kAll;
}

}  // namespace

TEST(TenantErrorTest, CodeCountMatchesAnchor) {
    EXPECT_EQ(static_cast<int>(AllCodes().size()), kTenantErrorCodeCount);
    EXPECT_EQ(kTenantErrorCodeCount, 24);
}

TEST(TenantErrorTest, EveryCodeHasUniqueWellFormedCxString) {
    const std::regex cx_re("^CX_ERR_[A-Z0-9_]+$");
    std::set<std::string> seen;
    for (auto code : AllCodes()) {
        const char* s = TenantErrorCodeString(code);
        ASSERT_NE(s, nullptr);
        std::string str(s);
        EXPECT_TRUE(std::regex_match(str, cx_re)) << "ill-formed: " << str;
        EXPECT_TRUE(seen.insert(str).second) << "duplicate CX string: " << str;
    }
    EXPECT_EQ(seen.size(), AllCodes().size());
}

TEST(TenantErrorTest, EveryCategoryIsOneOfFive) {
    for (auto code : AllCodes()) {
        ErrorCategory cat = GetTenantErrorInfo(code).category;
        bool valid = cat == ErrorCategory::kAuth || cat == ErrorCategory::kQuota ||
                     cat == ErrorCategory::kTransient || cat == ErrorCategory::kPermanent ||
                     cat == ErrorCategory::kTimeout;
        EXPECT_TRUE(valid) << TenantErrorCodeString(code);
    }
}

TEST(TenantErrorTest, OnlyTransactionFailedIsRetryable) {
    // Per sec 7, the sole retryable tenancy error is CX_ERR_TENANT_TRANSACTION_FAILED
    // (transient, retry_after_ms=100). Everything else is non-retryable.
    for (auto code : AllCodes()) {
        const TenantErrorInfo& info = GetTenantErrorInfo(code);
        if (code == TenantErrorCode::kTenantTransactionFailed) {
            EXPECT_TRUE(info.retryable);
            EXPECT_EQ(info.category, ErrorCategory::kTransient);
            ASSERT_TRUE(info.retry_after_ms.has_value());
            EXPECT_EQ(*info.retry_after_ms, 100);
        } else {
            EXPECT_FALSE(info.retryable) << TenantErrorCodeString(code);
            EXPECT_FALSE(info.retry_after_ms.has_value()) << TenantErrorCodeString(code);
        }
    }
}

TEST(TenantErrorTest, SpecificRowsMatchSpec) {
    EXPECT_STREQ(TenantErrorCodeString(TenantErrorCode::kNsUnauthorized), "CX_ERR_NS_UNAUTHORIZED");
    EXPECT_EQ(GetTenantErrorInfo(TenantErrorCode::kNsUnauthorized).category, ErrorCategory::kAuth);

    EXPECT_STREQ(TenantErrorCodeString(TenantErrorCode::kTenantQuotaExceeded), "CX_ERR_TENANT_QUOTA_EXCEEDED");
    EXPECT_EQ(GetTenantErrorInfo(TenantErrorCode::kTenantQuotaExceeded).category, ErrorCategory::kQuota);

    EXPECT_STREQ(TenantErrorCodeString(TenantErrorCode::kAdminKeyRequired), "CX_ERR_ADMIN_KEY_REQUIRED");
    EXPECT_EQ(GetTenantErrorInfo(TenantErrorCode::kAdminKeyRequired).category, ErrorCategory::kAuth);
}

TEST(TenantErrorTest, RequiredStructuredDataKeysMatchSpec) {
    using V = std::vector<std::string>;
    EXPECT_EQ(RequiredStructuredDataKeys(TenantErrorCode::kNsUnauthorized),
              (V{"ns_id", "tenant_id", "user_id", "required_action"}));
    EXPECT_EQ(RequiredStructuredDataKeys(TenantErrorCode::kTenantQuotaExceeded),
              (V{"tenant_id", "quota_type", "current_usage", "quota_limit"}));
    EXPECT_EQ(RequiredStructuredDataKeys(TenantErrorCode::kAclDuplicateGrant),
              (V{"ns_id", "grantee_tenant_id", "grantee_user_id", "existing_role"}));
    EXPECT_EQ(RequiredStructuredDataKeys(TenantErrorCode::kCallerRoleInsufficient),
              (V{"tenant_id", "caller_role", "required_min_role", "action"}));
    // Every code must declare at least one required structured_data key (sec 7 has no
    // empty rows for tenancy).
    for (auto code : AllCodes()) {
        EXPECT_FALSE(RequiredStructuredDataKeys(code).empty()) << TenantErrorCodeString(code);
    }
}

TEST(TenantErrorTest, HasRequiredStructuredDataChecksKeys) {
    nlohmann::json full = {
        {"ns_id", "ns-1"}, {"tenant_id", "t-1"}, {"user_id", "u-1"}, {"required_action", "read"}};
    EXPECT_TRUE(HasRequiredStructuredData(TenantErrorCode::kNsUnauthorized, full));

    nlohmann::json missing = {{"ns_id", "ns-1"}};
    EXPECT_FALSE(HasRequiredStructuredData(TenantErrorCode::kNsUnauthorized, missing));
}

TEST(TenantErrorTest, MakeTenantErrorBuildsAgentFriendlyBody) {
    nlohmann::json sd = {{"tenant_id", "t-1"}};
    auto err = MakeTenantError(TenantErrorCode::kTenantNotFound, sd, "no such tenant");
    EXPECT_EQ(err.code, "CX_ERR_TENANT_NOT_FOUND");
    EXPECT_EQ(err.message, "no such tenant");
    EXPECT_FALSE(err.retryable);
    EXPECT_EQ(err.category, ErrorCategory::kPermanent);
    EXPECT_FALSE(err.retry_after_ms.has_value());
    ASSERT_TRUE(err.structured_data.has_value());
    EXPECT_EQ((*err.structured_data)["tenant_id"], "t-1");

    // Empty message falls back to the CX code (mirrors MakeAuthError).
    auto err2 = MakeTenantError(TenantErrorCode::kNsNotFound);
    EXPECT_EQ(err2.message, "CX_ERR_NS_NOT_FOUND");

    // Retryable transaction error carries retry_after_ms from the registry.
    auto err3 = MakeTenantError(TenantErrorCode::kTenantTransactionFailed,
                                {{"tenant_id", "t"}, {"operation", "insert"}, {"db_error", "x"}});
    EXPECT_TRUE(err3.retryable);
    ASSERT_TRUE(err3.retry_after_ms.has_value());
    EXPECT_EQ(*err3.retry_after_ms, 100);
}

TEST(TenantErrorTest, TenantStatusEmbedsCxToken) {
    auto st = TenantStatus(TenantErrorCode::kCallerNotMember, "u-1 not in t-1");
    EXPECT_FALSE(st.ok());
    EXPECT_EQ(st.code(), cortrix::StatusCode::kPermissionDenied);
    // The CX token is recoverable from the message prefix.
    EXPECT_NE(st.message().find("CX_ERR_CALLER_NOT_MEMBER"), std::string::npos);

    auto st2 = TenantStatus(TenantErrorCode::kTenantNotFound);
    EXPECT_EQ(st2.code(), cortrix::StatusCode::kNotFound);

    auto st3 = TenantStatus(TenantErrorCode::kAdminKeyRequired);
    EXPECT_EQ(st3.code(), cortrix::StatusCode::kUnauthenticated);
}
