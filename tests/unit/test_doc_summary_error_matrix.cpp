#include <gtest/gtest.h>

#include <optional>
#include <regex>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cortrix/agent_friendly/error.h"
#include "cortrix/common/status.h"
#include "cortrix/doc_summary/doc_summary_error.h"

// Exhaustive error-registry matrix for doc_summary (src/doc_summary/doc_summary_error.*).
// One TEST_P case per DocSummaryErrorCode. Unique suite name (DocSummaryErrorMatrix).
namespace cortrix::doc_summary {
namespace {

using agent_friendly::ErrorCategory;

const std::vector<DocSummaryErrorCode>& AllCodes() {
    static const std::vector<DocSummaryErrorCode> codes = {
        DocSummaryErrorCode::kLlmTimeout,
        DocSummaryErrorCode::kLlmInvalidOutput,
        DocSummaryErrorCode::kLlmBudgetExceeded,
        DocSummaryErrorCode::kDocTooLarge,
        DocSummaryErrorCode::kSchemaVersionMismatch,
        DocSummaryErrorCode::kFallbackFailed,
        DocSummaryErrorCode::kFts5FallbackFailed,
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

class DocSummaryErrorMatrix : public ::testing::TestWithParam<DocSummaryErrorCode> {};

// CX_ERR_F41_* token shape.
TEST_P(DocSummaryErrorMatrix, CodeStringWellFormed) {
    static const std::regex kPattern("^CX_(ERR|WARN)_F41_[A-Z][A-Z0-9_]*$");
    const std::string cx = DocSummaryErrorCodeString(GetParam());
    EXPECT_TRUE(std::regex_match(cx, kPattern)) << "bad code string: " << cx;
    EXPECT_EQ(cx, std::string(GetDocSummaryErrorInfo(GetParam()).cx_code));
}

TEST_P(DocSummaryErrorMatrix, CategoryInValidSet) {
    const DocSummaryErrorInfo& info = GetDocSummaryErrorInfo(GetParam());
    EXPECT_EQ(ValidCategoryStrings().count(agent_friendly::ToString(info.category)), 1u)
        << "code " << info.cx_code;
}

// retry_after_ms present implies retryable (universal). Here the reverse also holds
// (every retryable doc summary code carries a backoff, incl. the quota-category budget code).
TEST_P(DocSummaryErrorMatrix, RetryAfterImpliesRetryable) {
    const DocSummaryErrorInfo& info = GetDocSummaryErrorInfo(GetParam());
    if (info.retry_after_ms.has_value()) {
        EXPECT_TRUE(info.retryable) << info.cx_code;
        EXPECT_GT(*info.retry_after_ms, 0) << info.cx_code;
    }
    EXPECT_EQ(info.retryable, info.retry_after_ms.has_value()) << info.cx_code;
}

TEST_P(DocSummaryErrorMatrix, RequiredStructuredDataContract) {
    const DocSummaryErrorCode code = GetParam();
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

TEST_P(DocSummaryErrorMatrix, MakeErrorRoundTrips) {
    const DocSummaryErrorCode code = GetParam();
    const DocSummaryErrorInfo& info = GetDocSummaryErrorInfo(code);
    nlohmann::json sd = {{"probe", 1}};

    auto err = MakeDocSummaryError(code, sd, "human detail");
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

TEST_P(DocSummaryErrorMatrix, EmptyMessageFallsBackToCode) {
    const DocSummaryErrorCode code = GetParam();
    auto err = MakeDocSummaryError(code);
    EXPECT_EQ(err.message, std::string(DocSummaryErrorCodeString(code)));
}

TEST_P(DocSummaryErrorMatrix, StatusCodeBucketAndBridge) {
    const DocSummaryErrorCode code = GetParam();
    const StatusCode sc = DocSummaryErrorToStatusCode(code);
    EXPECT_EQ(ValidStatusCodes().count(sc), 1u);
    EXPECT_NE(sc, StatusCode::kOk);

    Status s = DocSummaryStatus(code, "detail-z");
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), sc);
    EXPECT_NE(s.message().find(DocSummaryErrorCodeString(code)), std::string::npos);
    EXPECT_NE(s.message().find("detail-z"), std::string::npos);

    Status bare = DocSummaryStatus(code);
    EXPECT_EQ(bare.message(), std::string(DocSummaryErrorCodeString(code)));
}

INSTANTIATE_TEST_SUITE_P(AllDocSummaryCodes, DocSummaryErrorMatrix,
                         ::testing::ValuesIn(AllCodes()));

TEST(DocSummaryErrorMatrixGuard, CodeStringsAreUniqueAndCountMatches) {
    std::set<std::string> seen;
    for (DocSummaryErrorCode code : AllCodes()) {
        EXPECT_TRUE(seen.insert(DocSummaryErrorCodeString(code)).second)
            << "duplicate: " << DocSummaryErrorCodeString(code);
    }
    EXPECT_EQ(seen.size(), AllCodes().size());
    EXPECT_EQ(static_cast<int>(AllCodes().size()), kDocSummaryErrorCodeCount);
}

}  // namespace
}  // namespace cortrix::doc_summary
