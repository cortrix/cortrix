#include <gtest/gtest.h>

#include <optional>
#include <regex>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cortrix/agent_friendly/error.h"
#include "cortrix/common/status.h"
#include "cortrix/import/import_error.h"

// Exhaustive error-registry matrix for F16a DB-import (src/import/import_error.*).
// One TEST_P case per F16aErrorCode. Unique suite name (F16aErrorMatrix).
namespace cortrix::import {
namespace {

using agent_friendly::ErrorCategory;

const std::vector<F16aErrorCode>& AllCodes() {
    static const std::vector<F16aErrorCode> codes = {
        F16aErrorCode::kConnectionFailed,
        F16aErrorCode::kAuthDenied,
        F16aErrorCode::kInvalidSql,
        F16aErrorCode::kTimeout,
        F16aErrorCode::kRowsLimitExceeded,
        F16aErrorCode::kCrossTenantRef,
        F16aErrorCode::kInternal,
    };
    return codes;
}

const std::set<std::string>& ValidCategoryStrings() {
    static const std::set<std::string> kFive = {"auth", "quota", "transient",
                                                "permanent", "timeout"};
    return kFive;
}

const std::set<StatusCode>& ValidStatusCodes() {
    static const std::set<StatusCode> kCodes = {
        StatusCode::kInvalidArgument, StatusCode::kNotFound,
        StatusCode::kAlreadyExists,   StatusCode::kPermissionDenied,
        StatusCode::kUnauthenticated, StatusCode::kInternal,
        StatusCode::kUnavailable};
    return kCodes;
}

class F16aErrorMatrix : public ::testing::TestWithParam<F16aErrorCode> {};

TEST_P(F16aErrorMatrix, CodeStringWellFormed) {
    static const std::regex kPattern("^CX_(ERR|WARN)_F16A_[A-Z][A-Z0-9_]*$");
    const std::string cx = F16aErrorCodeString(GetParam());
    EXPECT_TRUE(std::regex_match(cx, kPattern)) << "bad code string: " << cx;
    EXPECT_EQ(cx, std::string(GetF16aErrorInfo(GetParam()).cx_code));
}

TEST_P(F16aErrorMatrix, CategoryInValidSet) {
    const F16aErrorInfo& info = GetF16aErrorInfo(GetParam());
    EXPECT_EQ(ValidCategoryStrings().count(agent_friendly::ToString(info.category)), 1u)
        << "code " << info.cx_code;
}

// retry_after_ms present implies retryable; here every retryable F16a code also
// carries a positive backoff (module-uniform).
TEST_P(F16aErrorMatrix, RetryAfterImpliesRetryable) {
    const F16aErrorInfo& info = GetF16aErrorInfo(GetParam());
    if (info.retry_after_ms.has_value()) {
        EXPECT_TRUE(info.retryable) << info.cx_code;
        EXPECT_GT(*info.retry_after_ms, 0) << info.cx_code;
    }
    EXPECT_EQ(info.retryable, info.retry_after_ms.has_value()) << info.cx_code;
}

TEST_P(F16aErrorMatrix, HttpStatusInValidRange) {
    const F16aErrorInfo& info = GetF16aErrorInfo(GetParam());
    EXPECT_GE(info.http_status, 200);
    EXPECT_LT(info.http_status, 600);
}

TEST_P(F16aErrorMatrix, RequiredStructuredDataContract) {
    const F16aErrorCode code = GetParam();
    const std::vector<std::string>& keys = RequiredStructuredDataKeys(code);

    nlohmann::json full = nlohmann::json::object();
    for (const std::string& k : keys) full[k] = "v";
    EXPECT_TRUE(HasRequiredStructuredData(code, full));

    if (!keys.empty()) {
        nlohmann::json missing = full;
        missing.erase(keys.front());
        EXPECT_FALSE(HasRequiredStructuredData(code, missing)) << keys.front();
    }

    EXPECT_EQ(HasRequiredStructuredData(code, nlohmann::json("x")), keys.empty());
    EXPECT_EQ(HasRequiredStructuredData(code, nlohmann::json(nullptr)), keys.empty());
}

TEST_P(F16aErrorMatrix, MakeErrorRoundTrips) {
    const F16aErrorCode code = GetParam();
    const F16aErrorInfo& info = GetF16aErrorInfo(code);
    nlohmann::json sd = {{"probe", 1}};

    auto err = MakeF16aError(code, sd, "human detail");
    EXPECT_EQ(err.code, std::string(info.cx_code));
    EXPECT_EQ(err.category, info.category);
    EXPECT_EQ(err.retryable, info.retryable);
    EXPECT_EQ(err.retry_after_ms, info.retry_after_ms);
    EXPECT_EQ(err.message, "human detail");
    ASSERT_TRUE(err.structured_data.has_value());
    EXPECT_EQ((*err.structured_data)["probe"], 1);

    nlohmann::json body = agent_friendly::ToJson(err);
    EXPECT_EQ(body["code"], std::string(info.cx_code));
    EXPECT_EQ(body["category"], agent_friendly::ToString(info.category));
    EXPECT_EQ(body["retryable"], info.retryable);
    if (info.retry_after_ms.has_value()) {
        EXPECT_EQ(body["retry_after_ms"], *info.retry_after_ms);
    } else {
        EXPECT_TRUE(body["retry_after_ms"].is_null());
    }
    EXPECT_EQ(body["structured_data"]["probe"], 1);
}

TEST_P(F16aErrorMatrix, EmptyMessageFallsBackToCode) {
    const F16aErrorCode code = GetParam();
    auto err = MakeF16aError(code);
    EXPECT_EQ(err.message, std::string(F16aErrorCodeString(code)));
}

TEST_P(F16aErrorMatrix, StatusCodeBucketAndBridge) {
    const F16aErrorCode code = GetParam();
    const StatusCode sc = F16aErrorToStatusCode(code);
    EXPECT_EQ(ValidStatusCodes().count(sc), 1u);
    EXPECT_NE(sc, StatusCode::kOk);

    Status s = F16aStatus(code, "detail-z");
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), sc);
    EXPECT_NE(s.message().find(F16aErrorCodeString(code)), std::string::npos);
    EXPECT_NE(s.message().find("detail-z"), std::string::npos);

    Status bare = F16aStatus(code);
    EXPECT_EQ(bare.message(), std::string(F16aErrorCodeString(code)));
}

// The throwing form carries the same Agent-friendly error as the factory.
TEST_P(F16aErrorMatrix, ExceptionCarriesError) {
    const F16aErrorCode code = GetParam();
    const F16aErrorInfo& info = GetF16aErrorInfo(code);
    try {
        throw F16aException(code, nlohmann::json::object(), "boom");
    } catch (const F16aException& ex) {
        EXPECT_EQ(ex.code(), code);
        const auto& err = ex.GetError();
        EXPECT_EQ(err.code, std::string(info.cx_code));
        EXPECT_EQ(err.category, info.category);
        EXPECT_EQ(err.retryable, info.retryable);
        EXPECT_EQ(err.message, "boom");
    }
}

INSTANTIATE_TEST_SUITE_P(AllF16aCodes, F16aErrorMatrix,
                         ::testing::ValuesIn(AllCodes()));

TEST(F16aErrorMatrixGuard, CodeStringsAreUniqueAndCountMatches) {
    std::set<std::string> seen;
    for (F16aErrorCode code : AllCodes()) {
        EXPECT_TRUE(seen.insert(F16aErrorCodeString(code)).second)
            << "duplicate: " << F16aErrorCodeString(code);
    }
    EXPECT_EQ(seen.size(), AllCodes().size());
    EXPECT_EQ(static_cast<int>(AllCodes().size()), kF16aErrorCodeCount);
}

}  // namespace
}  // namespace cortrix::import
