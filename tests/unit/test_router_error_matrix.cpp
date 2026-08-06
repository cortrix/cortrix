#include <gtest/gtest.h>

#include <regex>
#include <set>
#include <string>
#include <vector>

#include "cortrix/agent_friendly/error.h"
#include "cortrix/query/router_error.h"

// Exhaustive parameterized error-registry sweep for query-router
// (all codes). Distinct suite name (RouterErrorMatrix) from test_router_error.cpp.
namespace cortrix::query {
namespace {

using agent_friendly::ErrorCategory;

const RouterErrorCode kAll[] = {
    RouterErrorCode::kClassifierLoadFailed,
    RouterErrorCode::kInferenceFailed,
    RouterErrorCode::kForceRouteInvalid,
    RouterErrorCode::kFallbackTriggered,
};

bool IsValidCategory(ErrorCategory c) {
    return c == ErrorCategory::kAuth || c == ErrorCategory::kQuota ||
           c == ErrorCategory::kTransient || c == ErrorCategory::kPermanent ||
           c == ErrorCategory::kTimeout;
}

class RouterErrorMatrix : public ::testing::TestWithParam<RouterErrorCode> {};

TEST_P(RouterErrorMatrix, CodeStringFormat) {
    const RouterErrorCode code = GetParam();
    const std::string s = RouterErrorCodeString(code);
    EXPECT_FALSE(s.empty());
    EXPECT_EQ(s.rfind("CX_ERR_ROUTER_", 0), 0u) << s;
    EXPECT_TRUE(std::regex_match(s, std::regex("^CX_ERR_ROUTER_[A-Z0-9_]+$"))) << s;
    EXPECT_STREQ(GetRouterErrorInfo(code).cx_code, s.c_str());
}

TEST_P(RouterErrorMatrix, CategoryAndRetryInvariant) {
    const RouterErrorCode code = GetParam();
    const RouterErrorInfo& info = GetRouterErrorInfo(code);
    EXPECT_TRUE(IsValidCategory(info.category));
    // Non-retryable codes carry no retry hint. (kFallbackTriggered is retryable
    // with a null interval by design, so the converse direction is not asserted.)
    if (!info.retryable) {
        EXPECT_FALSE(info.retry_after_ms.has_value()) << info.cx_code;
    }
    if (info.retry_after_ms.has_value()) {
        EXPECT_GT(*info.retry_after_ms, 0) << info.cx_code;
    }
}

TEST_P(RouterErrorMatrix, RequiredStructuredDataArms) {
    const RouterErrorCode code = GetParam();
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

TEST_P(RouterErrorMatrix, MakeRoundTripAndJson) {
    const RouterErrorCode code = GetParam();
    const RouterErrorInfo& info = GetRouterErrorInfo(code);

    nlohmann::json sd = nlohmann::json::object();
    for (const std::string& k : RequiredStructuredDataKeys(code)) sd[k] = "v";

    auto err = MakeRouterError(code, sd, "human detail");
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

TEST_P(RouterErrorMatrix, EmptyMessageFallsBackToCode) {
    const RouterErrorCode code = GetParam();
    auto err = MakeRouterError(code);
    EXPECT_EQ(err.message, std::string(RouterErrorCodeString(code)));
}

TEST_P(RouterErrorMatrix, StatusBridge) {
    const RouterErrorCode code = GetParam();
    const StatusCode mapped = RouterErrorToStatusCode(code);
    EXPECT_NE(mapped, StatusCode::kOk);

    Status s = RouterStatus(code, "detail-x");
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), mapped);
    EXPECT_NE(s.message().find(RouterErrorCodeString(code)), std::string::npos);
    EXPECT_NE(s.message().find("detail-x"), std::string::npos);
}

TEST(RouterErrorMatrixAggregate, CountAndUniqueCodes) {
    EXPECT_EQ(kRouterErrorCodeCount, 4);
    EXPECT_EQ(std::size(kAll), static_cast<size_t>(kRouterErrorCodeCount));
    std::set<std::string> seen;
    for (RouterErrorCode c : kAll) {
        EXPECT_TRUE(seen.insert(RouterErrorCodeString(c)).second);
    }
    EXPECT_EQ(seen.size(), static_cast<size_t>(kRouterErrorCodeCount));
}

INSTANTIATE_TEST_SUITE_P(AllCodes, RouterErrorMatrix, ::testing::ValuesIn(kAll));

}  // namespace
}  // namespace cortrix::query
