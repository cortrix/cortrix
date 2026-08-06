#include <gtest/gtest.h>

#include <optional>
#include <regex>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cortrix/agent_friendly/error.h"
#include "cortrix/auth/auth_error.h"

// Auth S1 coverage: the auth error codes — CX_ERR_ identity, category
// mapping, retryability, structured_data keys, and the MakeAuthError boundary
// factory (incl. the live retry_after override for ACCOUNT_LOCKED / RATE_LIMITED).
namespace cortrix::auth {
namespace {

using agent_friendly::ErrorCategory;

// All codes in enum order. Explicit (not a loop over ints) so the test documents
// the locked set and fails to compile if an enumerator is removed.
const std::vector<AuthErrorCode>& AllCodes() {
    static const std::vector<AuthErrorCode> codes = {
        AuthErrorCode::kEmailAlreadyExists,
        AuthErrorCode::kInvalidCredentials,
        AuthErrorCode::kAccountLocked,
        AuthErrorCode::kAccountDisabled,
        AuthErrorCode::kTokenExpired,
        AuthErrorCode::kTokenRevoked,
        AuthErrorCode::kInvalidRefreshToken,
        AuthErrorCode::kTokenVerificationFailed,
        AuthErrorCode::kInvalidResetCode,
        AuthErrorCode::kBootstrapTokenInvalid,
        AuthErrorCode::kInvalidApiKey,
        AuthErrorCode::kAdminRequired,
        AuthErrorCode::kEmailSendFailed,
        AuthErrorCode::kBcryptTimeout,
        AuthErrorCode::kJwtInitFailed,
        AuthErrorCode::kUserNotFound,
        AuthErrorCode::kUserEmailExists,
        AuthErrorCode::kUserValidation,
        AuthErrorCode::kInvalidRequest,
        AuthErrorCode::kUnauthorized,
        AuthErrorCode::kNotFound,
        AuthErrorCode::kRateLimited,
        AuthErrorCode::kInternalError,
        AuthErrorCode::kServiceUnavailable,
    };
    return codes;
}

TEST(AuthErrorTest, CodeCountMatchesAnchor) {
    EXPECT_EQ(AllCodes().size(), static_cast<size_t>(kAuthErrorCodeCount));
    EXPECT_EQ(kAuthErrorCodeCount, 24);
}

// Every code's CX_ERR_* string is unique and matches the ErrorResponseV1 pattern
// ^CX_ERR_[A-Z][A-Z_]*$ (ARCH; auth V3-resolution-14 added AUTH prefix).
TEST(AuthErrorTest, EveryCodeHasUniqueWellFormedCxString) {
    static const std::regex kPattern("^CX_ERR_[A-Z][A-Z_]*$");
    std::set<std::string> seen;
    for (AuthErrorCode code : AllCodes()) {
        std::string cx = AuthErrorCodeString(code);
        EXPECT_TRUE(std::regex_match(cx, kPattern)) << "bad code string: " << cx;
        EXPECT_TRUE(seen.insert(cx).second) << "duplicate code string: " << cx;
    }
    EXPECT_EQ(seen.size(), static_cast<size_t>(kAuthErrorCodeCount));
}

// The 14 auth-domain codes carry the CX_ERR_AUTH_ second-level prefix; the
// project-shared + admin/users codes intentionally do NOT (master-table consolidated under,
// V14 J6). Guards the prefix decision against accidental drift.
TEST(AuthErrorTest, AuthDomainCodesCarryAuthPrefix) {
    const std::set<AuthErrorCode> kAuthPrefixed = {
        AuthErrorCode::kEmailAlreadyExists, AuthErrorCode::kInvalidCredentials,
        AuthErrorCode::kAccountDisabled,    AuthErrorCode::kTokenExpired,
        AuthErrorCode::kTokenRevoked,       AuthErrorCode::kInvalidRefreshToken,
        AuthErrorCode::kTokenVerificationFailed, AuthErrorCode::kInvalidResetCode,
        AuthErrorCode::kBootstrapTokenInvalid,   AuthErrorCode::kInvalidApiKey,
        AuthErrorCode::kAdminRequired,      AuthErrorCode::kEmailSendFailed,
        AuthErrorCode::kBcryptTimeout,      AuthErrorCode::kJwtInitFailed,
    };
    EXPECT_EQ(kAuthPrefixed.size(), 14u);
    for (AuthErrorCode code : kAuthPrefixed) {
        std::string cx = AuthErrorCodeString(code);
        EXPECT_EQ(cx.rfind("CX_ERR_AUTH_", 0), 0u) << cx << " should carry AUTH prefix";
    }
    // Shared codes keep the generic prefix (NOT CX_ERR_AUTH_).
    for (AuthErrorCode code : {AuthErrorCode::kAccountLocked, AuthErrorCode::kUnauthorized,
                               AuthErrorCode::kNotFound, AuthErrorCode::kRateLimited,
                               AuthErrorCode::kInvalidRequest, AuthErrorCode::kInternalError,
                               AuthErrorCode::kServiceUnavailable,
                               AuthErrorCode::kUserNotFound, AuthErrorCode::kUserEmailExists,
                               AuthErrorCode::kUserValidation}) {
        std::string cx = AuthErrorCodeString(code);
        EXPECT_NE(cx.rfind("CX_ERR_AUTH_", 0), 0u) << cx << " should NOT carry AUTH prefix";
    }
}

// Every category is one of the 5 GEN-Agent enum values (also exercises ToString).
TEST(AuthErrorTest, EveryCategoryIsOneOfFive) {
    const std::set<std::string> kFive = {"auth", "quota", "transient",
                                         "permanent", "timeout"};
    for (AuthErrorCode code : AllCodes()) {
        const AuthErrorInfo& info = GetAuthErrorInfo(code);
        EXPECT_EQ(kFive.count(agent_friendly::ToString(info.category)), 1u)
            << "code " << info.cx_code << " has out-of-range category";
    }
}

// Spot-check the exact rows downstream consumers depend on. NOTE: unlike the
// catalog table, auth's ACCOUNT_LOCKED / RATE_LIMITED are retryable+quota with a
// *live* retry_after (static default = nullopt, injected per-call) — asserted below.
TEST(AuthErrorTest, SpecificRowsMatchSpec) {
    auto check = [](AuthErrorCode c, const char* cx, ErrorCategory cat, bool retry,
                    std::optional<int> after) {
        const AuthErrorInfo& i = GetAuthErrorInfo(c);
        EXPECT_STREQ(i.cx_code, cx);
        EXPECT_EQ(i.category, cat);
        EXPECT_EQ(i.retryable, retry);
        EXPECT_EQ(i.retry_after_ms, after);
    };
    check(AuthErrorCode::kInvalidCredentials, "CX_ERR_AUTH_INVALID_CREDENTIALS",
          ErrorCategory::kAuth, false, std::nullopt);
    check(AuthErrorCode::kAccountLocked, "CX_ERR_ACCOUNT_LOCKED",
          ErrorCategory::kQuota, true, std::nullopt);  // live retry_after per-call
    check(AuthErrorCode::kTokenExpired, "CX_ERR_AUTH_TOKEN_EXPIRED",
          ErrorCategory::kAuth, false, std::nullopt);
    check(AuthErrorCode::kTokenVerificationFailed, "CX_ERR_AUTH_TOKEN_VERIFICATION_FAILED",
          ErrorCategory::kTransient, true, 5000);
    check(AuthErrorCode::kEmailSendFailed, "CX_ERR_AUTH_EMAIL_SEND_FAILED",
          ErrorCategory::kTransient, true, 30000);
    check(AuthErrorCode::kJwtInitFailed, "CX_ERR_AUTH_JWT_INIT_FAILED",
          ErrorCategory::kPermanent, false, std::nullopt);
    check(AuthErrorCode::kServiceUnavailable, "CX_ERR_SERVICE_UNAVAILABLE",
          ErrorCategory::kTransient, true, 10000);
}

// MakeAuthError fills the boundary error from the registry and attaches
// structured_data; ToJson then yields the Agent-friendly contract body shape.
TEST(AuthErrorTest, MakeAuthErrorBuildsAgentFriendlyBody) {
    nlohmann::json sd = {{"reason", "expired"}};
    auto err = MakeAuthError(AuthErrorCode::kInvalidResetCode, sd);
    EXPECT_EQ(err.code, "CX_ERR_AUTH_INVALID_RESET_CODE");
    EXPECT_FALSE(err.retryable);
    EXPECT_EQ(err.category, ErrorCategory::kPermanent);
    EXPECT_FALSE(err.retry_after_ms.has_value());
    ASSERT_TRUE(err.structured_data.has_value());
    EXPECT_EQ((*err.structured_data)["reason"], "expired");
    EXPECT_EQ(err.message, "CX_ERR_AUTH_INVALID_RESET_CODE");  // default → code string

    nlohmann::json body = agent_friendly::ToJson(err);
    EXPECT_EQ(body["code"], "CX_ERR_AUTH_INVALID_RESET_CODE");
    EXPECT_EQ(body["category"], "permanent");
    EXPECT_TRUE(body["retry_after_ms"].is_null());
}

// CX_ERR_ACCOUNT_LOCKED carries the *live* remaining-lock ms via the per-call
// override (auth retry_after_ms = remaining lockout ms). Without an override it stays
// null (the static row default).
TEST(AuthErrorTest, AccountLockedCarriesLiveRetryAfter) {
    nlohmann::json sd = {{"locked_until", "2026-05-14T10:35:00Z"},
                         {"attempts_remaining", 0}};
    auto err = MakeAuthError(AuthErrorCode::kAccountLocked, sd, /*message=*/"",
                             /*retry_after_override=*/723000);
    EXPECT_EQ(err.code, "CX_ERR_ACCOUNT_LOCKED");
    EXPECT_TRUE(err.retryable);
    EXPECT_EQ(err.category, ErrorCategory::kQuota);
    ASSERT_TRUE(err.retry_after_ms.has_value());
    EXPECT_EQ(*err.retry_after_ms, 723000);

    nlohmann::json body = agent_friendly::ToJson(err);
    EXPECT_EQ(body["retry_after_ms"], 723000);
    EXPECT_EQ(body["structured_data"]["attempts_remaining"], 0);

    // No override → null (table default).
    auto err2 = MakeAuthError(AuthErrorCode::kAccountLocked, sd);
    EXPECT_FALSE(err2.retry_after_ms.has_value());
}

// AuthStatus bridges to a coarse StatusCode while preserving the CX_ERR_ token in
// the message (so the API boundary can re-inflate the full Agent body).
TEST(AuthErrorTest, AuthStatusCarriesCodeTokenAndCoarseStatus) {
    Status s = AuthStatus(AuthErrorCode::kInvalidCredentials, "bad password");
    EXPECT_EQ(s.code(), StatusCode::kUnauthenticated);
    EXPECT_NE(s.message().find("CX_ERR_AUTH_INVALID_CREDENTIALS"), std::string::npos);
    EXPECT_NE(s.message().find("bad password"), std::string::npos);

    EXPECT_EQ(AuthErrorToStatusCode(AuthErrorCode::kUserNotFound), StatusCode::kNotFound);
    EXPECT_EQ(AuthErrorToStatusCode(AuthErrorCode::kEmailAlreadyExists),
              StatusCode::kAlreadyExists);
    EXPECT_EQ(AuthErrorToStatusCode(AuthErrorCode::kAdminRequired),
              StatusCode::kPermissionDenied);
    EXPECT_EQ(AuthErrorToStatusCode(AuthErrorCode::kJwtInitFailed), StatusCode::kInternal);
}

// HasRequiredStructuredData enforces the structured_data keys; the
// intentionally-empty codes (e.g. INVALID_CREDENTIALS — anti-enumeration) accept any object.
TEST(AuthErrorTest, RequiredStructuredDataKeysEnforced) {
    // kAdminRequired requires required_role + current_role.
    EXPECT_FALSE(HasRequiredStructuredData(
        AuthErrorCode::kAdminRequired, nlohmann::json{{"required_role", "admin"}}));
    EXPECT_TRUE(HasRequiredStructuredData(
        AuthErrorCode::kAdminRequired,
        nlohmann::json{{"required_role", "admin"}, {"current_role", "member"}}));

    // kInvalidCredentials has no required keys (empty object passes — anti-enumeration).
    EXPECT_TRUE(HasRequiredStructuredData(AuthErrorCode::kInvalidCredentials,
                                          nlohmann::json::object()));
}

}  // namespace
}  // namespace cortrix::auth
