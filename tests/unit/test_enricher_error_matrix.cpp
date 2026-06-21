#include <gtest/gtest.h>

#include <regex>
#include <set>
#include <string>
#include <vector>

#include "cortrix/agent_friendly/error.h"
#include "cortrix/spc_enricher/enricher_error.h"

// Exhaustive parameterized error-registry sweep for F03 enricher (§5.1, 6 codes).
// Distinct suite name (EnricherErrorMatrix) from test_enricher_error.cpp.
namespace cortrix::spc {
namespace {

using agent_friendly::ErrorCategory;

const EnricherErrorCode kAll[] = {
    EnricherErrorCode::kInitFailed,
    EnricherErrorCode::kLlmTimeout,
    EnricherErrorCode::kLlmApi,
    EnricherErrorCode::kParse,
    EnricherErrorCode::kBudget,
    EnricherErrorCode::kRateLimit,
};

bool IsValidCategory(ErrorCategory c) {
    return c == ErrorCategory::kAuth || c == ErrorCategory::kQuota ||
           c == ErrorCategory::kTransient || c == ErrorCategory::kPermanent ||
           c == ErrorCategory::kTimeout;
}

class EnricherErrorMatrix : public ::testing::TestWithParam<EnricherErrorCode> {};

TEST_P(EnricherErrorMatrix, CodeStringFormat) {
    const EnricherErrorCode code = GetParam();
    const std::string s = EnricherErrorCodeString(code);
    EXPECT_FALSE(s.empty());
    EXPECT_EQ(s.rfind("CX_ERR_ENRICHER_", 0), 0u) << s;
    EXPECT_TRUE(std::regex_match(s, std::regex("^CX_ERR_ENRICHER_[A-Z0-9_]+$"))) << s;
    EXPECT_STREQ(GetEnricherErrorInfo(code).cx_code, s.c_str());
}

TEST_P(EnricherErrorMatrix, CategoryAndRetryInvariant) {
    const EnricherErrorCode code = GetParam();
    const EnricherErrorInfo& info = GetEnricherErrorInfo(code);
    EXPECT_TRUE(IsValidCategory(info.category));
    if (!info.retryable) {
        EXPECT_FALSE(info.retry_after_ms.has_value()) << info.cx_code;
    }
    if (info.retry_after_ms.has_value()) {
        EXPECT_GT(*info.retry_after_ms, 0) << info.cx_code;
    }
}

TEST_P(EnricherErrorMatrix, RequiredStructuredDataArms) {
    const EnricherErrorCode code = GetParam();
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

TEST_P(EnricherErrorMatrix, MakeRoundTripAndJson) {
    const EnricherErrorCode code = GetParam();
    const EnricherErrorInfo& info = GetEnricherErrorInfo(code);

    nlohmann::json sd = nlohmann::json::object();
    for (const std::string& k : RequiredStructuredDataKeys(code)) sd[k] = "v";

    auto err = MakeEnricherError(code, sd, "human detail");
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

TEST_P(EnricherErrorMatrix, EmptyMessageFallsBackToCode) {
    const EnricherErrorCode code = GetParam();
    auto err = MakeEnricherError(code);
    EXPECT_EQ(err.message, std::string(EnricherErrorCodeString(code)));
}

TEST_P(EnricherErrorMatrix, StatusBridge) {
    const EnricherErrorCode code = GetParam();
    const StatusCode mapped = EnricherErrorToStatusCode(code);
    EXPECT_NE(mapped, StatusCode::kOk);

    Status s = EnricherStatus(code, "detail-x");
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), mapped);
    EXPECT_NE(s.message().find(EnricherErrorCodeString(code)), std::string::npos);
    EXPECT_NE(s.message().find("detail-x"), std::string::npos);
}

TEST(EnricherErrorMatrixAggregate, CountAndUniqueCodes) {
    EXPECT_EQ(kEnricherErrorCodeCount, 6);
    EXPECT_EQ(std::size(kAll), static_cast<size_t>(kEnricherErrorCodeCount));
    std::set<std::string> seen;
    for (EnricherErrorCode c : kAll) {
        EXPECT_TRUE(seen.insert(EnricherErrorCodeString(c)).second);
    }
    EXPECT_EQ(seen.size(), static_cast<size_t>(kEnricherErrorCodeCount));
}

INSTANTIATE_TEST_SUITE_P(AllCodes, EnricherErrorMatrix, ::testing::ValuesIn(kAll));

}  // namespace
}  // namespace cortrix::spc
