#include <gtest/gtest.h>

#include <regex>
#include <set>
#include <string>
#include <vector>

#include "cortrix/agent_friendly/error.h"
#include "cortrix/spc/contextual_error.h"

// Exhaustive parameterized error-registry sweep for contextual retrieval (§8,
// 5 codes). Distinct suite (ContextualErrorMatrix) from test_contextual_error.cpp.
namespace cortrix::spc {
namespace {

using agent_friendly::ErrorCategory;

const ContextualErrorCode kAll[] = {
    ContextualErrorCode::kLlmFailed,
    ContextualErrorCode::kBudgetExceeded,
    ContextualErrorCode::kPromptInjection,
    ContextualErrorCode::kEmbeddingFailed,
    ContextualErrorCode::kStartupNoLlm,
};

bool IsValidCategory(ErrorCategory c) {
    return c == ErrorCategory::kAuth || c == ErrorCategory::kQuota ||
           c == ErrorCategory::kTransient || c == ErrorCategory::kPermanent ||
           c == ErrorCategory::kTimeout;
}

class ContextualErrorMatrix : public ::testing::TestWithParam<ContextualErrorCode> {};

TEST_P(ContextualErrorMatrix, CodeStringFormat) {
    const ContextualErrorCode code = GetParam();
    const std::string s = ContextualErrorCodeString(code);
    EXPECT_FALSE(s.empty());
    EXPECT_EQ(s.rfind("CX_ERR_CONTEXTUAL_", 0), 0u) << s;
    EXPECT_TRUE(std::regex_match(s, std::regex("^CX_ERR_CONTEXTUAL_[A-Z0-9_]+$"))) << s;
    EXPECT_STREQ(GetContextualErrorInfo(code).cx_code, s.c_str());
}

TEST_P(ContextualErrorMatrix, CategoryAndRetryInvariant) {
    const ContextualErrorCode code = GetParam();
    const ContextualErrorInfo& info = GetContextualErrorInfo(code);
    EXPECT_TRUE(IsValidCategory(info.category));
    if (!info.retryable) {
        EXPECT_FALSE(info.retry_after_ms.has_value()) << info.cx_code;
    }
    if (info.retry_after_ms.has_value()) {
        EXPECT_GT(*info.retry_after_ms, 0) << info.cx_code;
    }
}

TEST_P(ContextualErrorMatrix, RequiredStructuredDataArms) {
    const ContextualErrorCode code = GetParam();
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

TEST_P(ContextualErrorMatrix, MakeRoundTripAndJson) {
    const ContextualErrorCode code = GetParam();
    const ContextualErrorInfo& info = GetContextualErrorInfo(code);

    nlohmann::json sd = nlohmann::json::object();
    for (const std::string& k : RequiredStructuredDataKeys(code)) sd[k] = "v";

    auto err = MakeContextualError(code, sd, "human detail");
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

TEST_P(ContextualErrorMatrix, EmptyMessageFallsBackToCode) {
    const ContextualErrorCode code = GetParam();
    auto err = MakeContextualError(code);
    EXPECT_EQ(err.message, std::string(ContextualErrorCodeString(code)));
}

TEST_P(ContextualErrorMatrix, StatusBridge) {
    const ContextualErrorCode code = GetParam();
    const StatusCode mapped = ContextualErrorToStatusCode(code);
    EXPECT_NE(mapped, StatusCode::kOk);

    Status s = ContextualStatus(code, "detail-x");
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), mapped);
    EXPECT_NE(s.message().find(ContextualErrorCodeString(code)), std::string::npos);
    EXPECT_NE(s.message().find("detail-x"), std::string::npos);
}

TEST(ContextualErrorMatrixAggregate, CountAndUniqueCodes) {
    EXPECT_EQ(kContextualErrorCodeCount, 5);
    EXPECT_EQ(std::size(kAll), static_cast<size_t>(kContextualErrorCodeCount));
    std::set<std::string> seen;
    for (ContextualErrorCode c : kAll) {
        EXPECT_TRUE(seen.insert(ContextualErrorCodeString(c)).second);
    }
    EXPECT_EQ(seen.size(), static_cast<size_t>(kContextualErrorCodeCount));
}

INSTANTIATE_TEST_SUITE_P(AllCodes, ContextualErrorMatrix, ::testing::ValuesIn(kAll));

}  // namespace
}  // namespace cortrix::spc
