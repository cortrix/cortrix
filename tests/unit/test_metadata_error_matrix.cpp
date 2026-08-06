#include <gtest/gtest.h>

#include <optional>
#include <regex>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cortrix/agent_friendly/error.h"
#include "cortrix/common/status.h"
#include "cortrix/metadata/metadata_error.h"

// Exhaustive error-registry matrix for META block metadata (src/metadata/metadata_error.*).
// One TEST_P case per MetadataErrorCode. Unique suite name (MetadataErrorMatrix).
namespace cortrix::metadata {
namespace {

using agent_friendly::ErrorCategory;

const std::vector<MetadataErrorCode>& AllCodes() {
    static const std::vector<MetadataErrorCode> codes = {
        MetadataErrorCode::kGenFailed,
        MetadataErrorCode::kPartial,
        MetadataErrorCode::kFieldImmutable,
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

class MetadataErrorMatrix : public ::testing::TestWithParam<MetadataErrorCode> {};

// CX_ERR_METADATA_* (errors) / CX_WARN_METADATA_* (the warning) token shape.
TEST_P(MetadataErrorMatrix, CodeStringWellFormed) {
    static const std::regex kPattern("^CX_(ERR|WARN)_METADATA_[A-Z][A-Z0-9_]*$");
    const std::string cx = MetadataErrorCodeString(GetParam());
    EXPECT_TRUE(std::regex_match(cx, kPattern)) << "bad code string: " << cx;
    EXPECT_EQ(cx, std::string(GetMetadataErrorInfo(GetParam()).cx_code));
}

TEST_P(MetadataErrorMatrix, CategoryInValidSet) {
    const MetadataErrorInfo& info = GetMetadataErrorInfo(GetParam());
    EXPECT_EQ(ValidCategoryStrings().count(agent_friendly::ToString(info.category)), 1u)
        << "code " << info.cx_code;
}

// retry_after_ms present implies retryable. All 3 metadata codes are non-retryable,
// so retry_after_ms is absent for every one.
TEST_P(MetadataErrorMatrix, RetryAfterImpliesRetryable) {
    const MetadataErrorInfo& info = GetMetadataErrorInfo(GetParam());
    if (info.retry_after_ms.has_value()) {
        EXPECT_TRUE(info.retryable) << info.cx_code;
        EXPECT_GT(*info.retry_after_ms, 0) << info.cx_code;
    }
    EXPECT_EQ(info.retryable, info.retry_after_ms.has_value()) << info.cx_code;
    EXPECT_FALSE(info.retryable) << info.cx_code;
}

// http_status column is a valid HTTP status (200 / 4xx / 5xx range).
TEST_P(MetadataErrorMatrix, HttpStatusInValidRange) {
    const MetadataErrorInfo& info = GetMetadataErrorInfo(GetParam());
    EXPECT_GE(info.http_status, 200);
    EXPECT_LT(info.http_status, 600);
}

TEST_P(MetadataErrorMatrix, RequiredStructuredDataContract) {
    const MetadataErrorCode code = GetParam();
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

TEST_P(MetadataErrorMatrix, MakeErrorRoundTrips) {
    const MetadataErrorCode code = GetParam();
    const MetadataErrorInfo& info = GetMetadataErrorInfo(code);
    nlohmann::json sd = {{"probe", 1}};

    auto err = MakeMetadataError(code, sd, "human detail");
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
    EXPECT_TRUE(body["retry_after_ms"].is_null());  // none retryable
    EXPECT_EQ(body["structured_data"]["probe"], 1);
}

TEST_P(MetadataErrorMatrix, EmptyMessageFallsBackToCode) {
    const MetadataErrorCode code = GetParam();
    auto err = MakeMetadataError(code);
    EXPECT_EQ(err.message, std::string(MetadataErrorCodeString(code)));
}

TEST_P(MetadataErrorMatrix, StatusCodeBucketAndBridge) {
    const MetadataErrorCode code = GetParam();
    const StatusCode sc = MetadataErrorToStatusCode(code);
    EXPECT_EQ(ValidStatusCodes().count(sc), 1u);
    EXPECT_NE(sc, StatusCode::kOk);

    Status s = MetadataStatus(code, "detail-z");
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), sc);
    EXPECT_NE(s.message().find(MetadataErrorCodeString(code)), std::string::npos);
    EXPECT_NE(s.message().find("detail-z"), std::string::npos);

    Status bare = MetadataStatus(code);
    EXPECT_EQ(bare.message(), std::string(MetadataErrorCodeString(code)));
}

INSTANTIATE_TEST_SUITE_P(AllMetadataCodes, MetadataErrorMatrix,
                         ::testing::ValuesIn(AllCodes()));

TEST(MetadataErrorMatrixGuard, CodeStringsAreUniqueAndCountMatches) {
    std::set<std::string> seen;
    for (MetadataErrorCode code : AllCodes()) {
        EXPECT_TRUE(seen.insert(MetadataErrorCodeString(code)).second)
            << "duplicate: " << MetadataErrorCodeString(code);
    }
    EXPECT_EQ(seen.size(), AllCodes().size());
    EXPECT_EQ(static_cast<int>(AllCodes().size()), kMetadataErrorCodeCount);
}

}  // namespace
}  // namespace cortrix::metadata
