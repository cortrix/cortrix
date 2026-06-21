#include <gtest/gtest.h>

#include <regex>
#include <set>
#include <string>
#include <vector>

#include "cortrix/agent_friendly/error.h"
#include "cortrix/tenant/tenant_error.h"

// Exhaustive parameterized error-registry sweep for P09 tenant (sec 7, 24 codes).
// Distinct suite name (TenantErrorMatrix) from test_tenant_error.cpp.
namespace cortrix::tenant {
namespace {

using agent_friendly::ErrorCategory;

const TenantErrorCode kAll[] = {
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

bool IsValidCategory(ErrorCategory c) {
    return c == ErrorCategory::kAuth || c == ErrorCategory::kQuota ||
           c == ErrorCategory::kTransient || c == ErrorCategory::kPermanent ||
           c == ErrorCategory::kTimeout;
}

class TenantErrorMatrix : public ::testing::TestWithParam<TenantErrorCode> {};

TEST_P(TenantErrorMatrix, CodeStringFormat) {
    const TenantErrorCode code = GetParam();
    const std::string s = TenantErrorCodeString(code);
    EXPECT_FALSE(s.empty());
    EXPECT_EQ(s.rfind("CX_ERR_", 0), 0u) << s;
    EXPECT_TRUE(std::regex_match(s, std::regex("^CX_ERR_[A-Z0-9_]+$"))) << s;
    EXPECT_STREQ(GetTenantErrorInfo(code).cx_code, s.c_str());
}

TEST_P(TenantErrorMatrix, CategoryAndRetryInvariant) {
    const TenantErrorCode code = GetParam();
    const TenantErrorInfo& info = GetTenantErrorInfo(code);
    EXPECT_TRUE(IsValidCategory(info.category));
    if (!info.retryable) {
        EXPECT_FALSE(info.retry_after_ms.has_value()) << info.cx_code;
    }
    if (info.retry_after_ms.has_value()) {
        EXPECT_GT(*info.retry_after_ms, 0) << info.cx_code;
    }
}

TEST_P(TenantErrorMatrix, RequiredStructuredDataArms) {
    const TenantErrorCode code = GetParam();
    const std::vector<std::string>& keys = RequiredStructuredDataKeys(code);

    nlohmann::json full = nlohmann::json::object();
    for (const std::string& k : keys) full[k] = "v";
    EXPECT_TRUE(HasRequiredStructuredData(code, full));

    if (!keys.empty()) {
        nlohmann::json missing = full;
        missing.erase(keys.front());
        EXPECT_FALSE(HasRequiredStructuredData(code, missing));
    }

    EXPECT_EQ(HasRequiredStructuredData(code, nlohmann::json("not-an-object")),
              keys.empty());
}

TEST_P(TenantErrorMatrix, MakeRoundTripAndJson) {
    const TenantErrorCode code = GetParam();
    const TenantErrorInfo& info = GetTenantErrorInfo(code);

    nlohmann::json sd = nlohmann::json::object();
    for (const std::string& k : RequiredStructuredDataKeys(code)) sd[k] = "v";

    auto err = MakeTenantError(code, sd, "human detail");
    EXPECT_EQ(err.code, info.cx_code);
    EXPECT_EQ(err.message, "human detail");
    EXPECT_EQ(err.retryable, info.retryable);
    EXPECT_EQ(err.category, info.category);
    EXPECT_EQ(err.retry_after_ms, info.retry_after_ms);

    nlohmann::json j = agent_friendly::ToJson(err);
    EXPECT_EQ(j["code"], std::string(info.cx_code));
    EXPECT_EQ(j["retryable"], info.retryable);
    EXPECT_EQ(j["category"], std::string(agent_friendly::ToString(info.category)));
    EXPECT_EQ(j["retry_after_ms"].is_null(), !info.retry_after_ms.has_value());
}

TEST_P(TenantErrorMatrix, EmptyMessageFallsBackToCode) {
    const TenantErrorCode code = GetParam();
    auto err = MakeTenantError(code);
    EXPECT_EQ(err.message, std::string(TenantErrorCodeString(code)));
}

TEST_P(TenantErrorMatrix, StatusBridge) {
    const TenantErrorCode code = GetParam();
    const StatusCode mapped = TenantErrorToStatusCode(code);
    EXPECT_NE(mapped, StatusCode::kOk);

    Status s = TenantStatus(code, "detail-x");
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), mapped);
    EXPECT_NE(s.message().find(TenantErrorCodeString(code)), std::string::npos);
    EXPECT_NE(s.message().find("detail-x"), std::string::npos);
}

TEST(TenantErrorMatrixAggregate, CountAndUniqueCodes) {
    EXPECT_EQ(kTenantErrorCodeCount, 24);
    EXPECT_EQ(std::size(kAll), static_cast<size_t>(kTenantErrorCodeCount));
    std::set<std::string> seen;
    for (TenantErrorCode c : kAll) {
        EXPECT_TRUE(seen.insert(TenantErrorCodeString(c)).second);
    }
    EXPECT_EQ(seen.size(), static_cast<size_t>(kTenantErrorCodeCount));
}

INSTANTIATE_TEST_SUITE_P(AllCodes, TenantErrorMatrix, ::testing::ValuesIn(kAll));

}  // namespace
}  // namespace cortrix::tenant
