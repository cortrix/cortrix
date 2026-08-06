#include <gtest/gtest.h>

#include <regex>
#include <set>
#include <string>
#include <vector>

#include "cortrix/agent_friendly/error.h"
#include "cortrix/query/cross_ns_error.h"

// Exhaustive parameterized error-registry sweep for cross-NS query (
// codes). The query namespace overloads RequiredStructuredDataKeys /
// HasRequiredStructuredData per enum; this TU only sees the CrossNs overloads.
namespace cortrix::query {
namespace {

using agent_friendly::ErrorCategory;

const CrossNsErrorCode kAll[] = {
    CrossNsErrorCode::kAuthInvalidCredentials,
    CrossNsErrorCode::kNsUnauthorized,
    CrossNsErrorCode::kTooManyNamespaces,
    CrossNsErrorCode::kNsTimeout,
    CrossNsErrorCode::kIndexCorrupt,
    CrossNsErrorCode::kScatterTimeout,
};

bool IsValidCategory(ErrorCategory c) {
    return c == ErrorCategory::kAuth || c == ErrorCategory::kQuota ||
           c == ErrorCategory::kTransient || c == ErrorCategory::kPermanent ||
           c == ErrorCategory::kTimeout;
}

class CrossNsErrorMatrix : public ::testing::TestWithParam<CrossNsErrorCode> {};

TEST_P(CrossNsErrorMatrix, CodeStringFormat) {
    const CrossNsErrorCode code = GetParam();
    const std::string s = CrossNsErrorCodeString(code);
    EXPECT_FALSE(s.empty());
    EXPECT_EQ(s.rfind("CX_ERR_", 0), 0u) << s;
    EXPECT_TRUE(std::regex_match(s, std::regex("^CX_ERR_[A-Z0-9_]+$"))) << s;
    EXPECT_STREQ(GetCrossNsErrorInfo(code).cx_code, s.c_str());
}

TEST_P(CrossNsErrorMatrix, CategoryAndRetryInvariant) {
    const CrossNsErrorCode code = GetParam();
    const CrossNsErrorInfo& info = GetCrossNsErrorInfo(code);
    EXPECT_TRUE(IsValidCategory(info.category));
    if (!info.retryable) {
        EXPECT_FALSE(info.retry_after_ms.has_value()) << info.cx_code;
    }
    if (info.retry_after_ms.has_value()) {
        EXPECT_GT(*info.retry_after_ms, 0) << info.cx_code;
    }
    EXPECT_GT(info.http_status, 0);
}

TEST_P(CrossNsErrorMatrix, RequiredStructuredDataArms) {
    const CrossNsErrorCode code = GetParam();
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

TEST_P(CrossNsErrorMatrix, MakeRoundTripAndJson) {
    const CrossNsErrorCode code = GetParam();
    const CrossNsErrorInfo& info = GetCrossNsErrorInfo(code);

    nlohmann::json sd = nlohmann::json::object();
    for (const std::string& k : RequiredStructuredDataKeys(code)) sd[k] = "v";

    auto err = MakeCrossNsError(code, sd, "human detail");
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

TEST_P(CrossNsErrorMatrix, EmptyMessageFallsBackToCode) {
    const CrossNsErrorCode code = GetParam();
    auto err = MakeCrossNsError(code);
    EXPECT_EQ(err.message, std::string(CrossNsErrorCodeString(code)));
}

TEST_P(CrossNsErrorMatrix, StatusBridge) {
    const CrossNsErrorCode code = GetParam();
    const StatusCode mapped = CrossNsErrorToStatusCode(code);
    EXPECT_NE(mapped, StatusCode::kOk);

    Status s = CrossNsStatus(code, "detail-x");
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), mapped);
    EXPECT_NE(s.message().find(CrossNsErrorCodeString(code)), std::string::npos);
    EXPECT_NE(s.message().find("detail-x"), std::string::npos);
}

TEST(CrossNsErrorMatrixAggregate, CountAndUniqueCodes) {
    EXPECT_EQ(kCrossNsErrorCodeCount, 6);
    EXPECT_EQ(std::size(kAll), static_cast<size_t>(kCrossNsErrorCodeCount));
    std::set<std::string> seen;
    for (CrossNsErrorCode c : kAll) {
        EXPECT_TRUE(seen.insert(CrossNsErrorCodeString(c)).second);
    }
    EXPECT_EQ(seen.size(), static_cast<size_t>(kCrossNsErrorCodeCount));
}

TEST(CrossNsErrorMatrixAggregate, NsUnauthorizedConvenienceBuilder) {
    auto err = MakeNsUnauthorizedError({"ns_a", "ns_b"});
    EXPECT_EQ(err.code, "CX_ERR_NS_UNAUTHORIZED");
    ASSERT_TRUE(err.structured_data.has_value());
    EXPECT_TRUE(
        HasRequiredStructuredData(CrossNsErrorCode::kNsUnauthorized, *err.structured_data));
    EXPECT_EQ((*err.structured_data)["unauthorized_namespaces"].size(), 2u);
}

INSTANTIATE_TEST_SUITE_P(AllCodes, CrossNsErrorMatrix, ::testing::ValuesIn(kAll));

}  // namespace
}  // namespace cortrix::query
