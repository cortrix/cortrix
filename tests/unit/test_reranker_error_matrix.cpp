#include <gtest/gtest.h>

#include <regex>
#include <set>
#include <string>
#include <vector>

#include "cortrix/agent_friendly/error.h"
#include "cortrix/reranker/reranker_error.h"

// Exhaustive parameterized error-registry sweep for reranker (codes).
namespace cortrix::reranker {
namespace {

using agent_friendly::ErrorCategory;

const RerankerErrorCode kAll[] = {
    RerankerErrorCode::kRerankerInitFailed,
    RerankerErrorCode::kConfigMismatch,
    RerankerErrorCode::kRerankerTaskTimeout,
    RerankerErrorCode::kChunkNotFound,
    RerankerErrorCode::kRerankerCircuitOpen,
};

bool IsValidCategory(ErrorCategory c) {
    return c == ErrorCategory::kAuth || c == ErrorCategory::kQuota ||
           c == ErrorCategory::kTransient || c == ErrorCategory::kPermanent ||
           c == ErrorCategory::kTimeout;
}

class RerankerErrorMatrix : public ::testing::TestWithParam<RerankerErrorCode> {};

TEST_P(RerankerErrorMatrix, CodeStringFormat) {
    const RerankerErrorCode code = GetParam();
    const std::string s = RerankerErrorCodeString(code);
    EXPECT_FALSE(s.empty());
    EXPECT_EQ(s.rfind("CX_ERR_", 0), 0u) << s;
    EXPECT_TRUE(std::regex_match(s, std::regex("^CX_ERR_[A-Z0-9_]+$"))) << s;
    EXPECT_STREQ(GetRerankerErrorInfo(code).cx_code, s.c_str());
}

TEST_P(RerankerErrorMatrix, CategoryAndRetryInvariant) {
    const RerankerErrorCode code = GetParam();
    const RerankerErrorInfo& info = GetRerankerErrorInfo(code);
    EXPECT_TRUE(IsValidCategory(info.category));
    if (!info.retryable) {
        EXPECT_FALSE(info.retry_after_ms.has_value()) << info.cx_code;
    }
    if (info.retry_after_ms.has_value()) {
        EXPECT_GT(*info.retry_after_ms, 0) << info.cx_code;
    }
}

TEST_P(RerankerErrorMatrix, RequiredStructuredDataArms) {
    const RerankerErrorCode code = GetParam();
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

TEST_P(RerankerErrorMatrix, MakeRoundTripAndJson) {
    const RerankerErrorCode code = GetParam();
    const RerankerErrorInfo& info = GetRerankerErrorInfo(code);

    nlohmann::json sd = nlohmann::json::object();
    for (const std::string& k : RequiredStructuredDataKeys(code)) sd[k] = "v";

    auto err = MakeRerankerError(code, sd, "human detail");
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

TEST_P(RerankerErrorMatrix, EmptyMessageFallsBackToCode) {
    const RerankerErrorCode code = GetParam();
    auto err = MakeRerankerError(code);
    EXPECT_EQ(err.message, std::string(RerankerErrorCodeString(code)));
}

TEST_P(RerankerErrorMatrix, StatusBridge) {
    const RerankerErrorCode code = GetParam();
    const StatusCode mapped = RerankerErrorToStatusCode(code);
    EXPECT_NE(mapped, StatusCode::kOk);

    Status s = RerankerStatus(code, "detail-x");
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), mapped);
    EXPECT_NE(s.message().find(RerankerErrorCodeString(code)), std::string::npos);
    EXPECT_NE(s.message().find("detail-x"), std::string::npos);
}

TEST(RerankerErrorMatrixAggregate, CountAndUniqueCodes) {
    EXPECT_EQ(kRerankerErrorCodeCount, 5);
    EXPECT_EQ(std::size(kAll), static_cast<size_t>(kRerankerErrorCodeCount));
    std::set<std::string> seen;
    for (RerankerErrorCode c : kAll) {
        EXPECT_TRUE(seen.insert(RerankerErrorCodeString(c)).second);
    }
    EXPECT_EQ(seen.size(), static_cast<size_t>(kRerankerErrorCodeCount));
}

INSTANTIATE_TEST_SUITE_P(AllCodes, RerankerErrorMatrix, ::testing::ValuesIn(kAll));

}  // namespace
}  // namespace cortrix::reranker
