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

// Exhaustive error-registry matrix for DB-import (src/import/import_error.*).
// One TEST_P case per ImportErrorCode. Unique suite name (ImportErrorMatrix).
namespace cortrix::import {
namespace {

using agent_friendly::ErrorCategory;

const std::vector<ImportErrorCode>& AllCodes() {
    static const std::vector<ImportErrorCode> codes = {
        ImportErrorCode::kConnectionFailed,
        ImportErrorCode::kAuthDenied,
        ImportErrorCode::kInvalidSql,
        ImportErrorCode::kTimeout,
        ImportErrorCode::kRowsLimitExceeded,
        ImportErrorCode::kCrossTenantRef,
        ImportErrorCode::kInternal,
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

class ImportErrorMatrix : public ::testing::TestWithParam<ImportErrorCode> {};

TEST_P(ImportErrorMatrix, CodeStringWellFormed) {
    static const std::regex kPattern("^CX_(ERR|WARN)_IMPORT_[A-Z][A-Z0-9_]*$");
    const std::string cx = ImportErrorCodeString(GetParam());
    EXPECT_TRUE(std::regex_match(cx, kPattern)) << "bad code string: " << cx;
    EXPECT_EQ(cx, std::string(GetImportErrorInfo(GetParam()).cx_code));
}

TEST_P(ImportErrorMatrix, CategoryInValidSet) {
    const ImportErrorInfo& info = GetImportErrorInfo(GetParam());
    EXPECT_EQ(ValidCategoryStrings().count(agent_friendly::ToString(info.category)), 1u)
        << "code " << info.cx_code;
}

// retry_after_ms present implies retryable; here every retryable DB import code also
// carries a positive backoff (module-uniform).
TEST_P(ImportErrorMatrix, RetryAfterImpliesRetryable) {
    const ImportErrorInfo& info = GetImportErrorInfo(GetParam());
    if (info.retry_after_ms.has_value()) {
        EXPECT_TRUE(info.retryable) << info.cx_code;
        EXPECT_GT(*info.retry_after_ms, 0) << info.cx_code;
    }
    EXPECT_EQ(info.retryable, info.retry_after_ms.has_value()) << info.cx_code;
}

TEST_P(ImportErrorMatrix, HttpStatusInValidRange) {
    const ImportErrorInfo& info = GetImportErrorInfo(GetParam());
    EXPECT_GE(info.http_status, 200);
    EXPECT_LT(info.http_status, 600);
}

TEST_P(ImportErrorMatrix, RequiredStructuredDataContract) {
    const ImportErrorCode code = GetParam();
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

TEST_P(ImportErrorMatrix, MakeErrorRoundTrips) {
    const ImportErrorCode code = GetParam();
    const ImportErrorInfo& info = GetImportErrorInfo(code);
    nlohmann::json sd = {{"probe", 1}};

    auto err = MakeImportError(code, sd, "human detail");
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

TEST_P(ImportErrorMatrix, EmptyMessageFallsBackToCode) {
    const ImportErrorCode code = GetParam();
    auto err = MakeImportError(code);
    EXPECT_EQ(err.message, std::string(ImportErrorCodeString(code)));
}

TEST_P(ImportErrorMatrix, StatusCodeBucketAndBridge) {
    const ImportErrorCode code = GetParam();
    const StatusCode sc = ImportErrorToStatusCode(code);
    EXPECT_EQ(ValidStatusCodes().count(sc), 1u);
    EXPECT_NE(sc, StatusCode::kOk);

    Status s = ImportStatus(code, "detail-z");
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), sc);
    EXPECT_NE(s.message().find(ImportErrorCodeString(code)), std::string::npos);
    EXPECT_NE(s.message().find("detail-z"), std::string::npos);

    Status bare = ImportStatus(code);
    EXPECT_EQ(bare.message(), std::string(ImportErrorCodeString(code)));
}

// The throwing form carries the same Agent-friendly error as the factory.
TEST_P(ImportErrorMatrix, ExceptionCarriesError) {
    const ImportErrorCode code = GetParam();
    const ImportErrorInfo& info = GetImportErrorInfo(code);
    try {
        throw ImportException(code, nlohmann::json::object(), "boom");
    } catch (const ImportException& ex) {
        EXPECT_EQ(ex.code(), code);
        const auto& err = ex.GetError();
        EXPECT_EQ(err.code, std::string(info.cx_code));
        EXPECT_EQ(err.category, info.category);
        EXPECT_EQ(err.retryable, info.retryable);
        EXPECT_EQ(err.message, "boom");
    }
}

INSTANTIATE_TEST_SUITE_P(AllImportCodes, ImportErrorMatrix,
                         ::testing::ValuesIn(AllCodes()));

TEST(ImportErrorMatrixGuard, CodeStringsAreUniqueAndCountMatches) {
    std::set<std::string> seen;
    for (ImportErrorCode code : AllCodes()) {
        EXPECT_TRUE(seen.insert(ImportErrorCodeString(code)).second)
            << "duplicate: " << ImportErrorCodeString(code);
    }
    EXPECT_EQ(seen.size(), AllCodes().size());
    EXPECT_EQ(static_cast<int>(AllCodes().size()), kImportErrorCodeCount);
}

}  // namespace
}  // namespace cortrix::import
