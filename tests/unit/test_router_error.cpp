#include <gtest/gtest.h>

#include <set>
#include <string>

#include "cortrix/query/router_error.h"

// Query routing coverage: the query-router error model (template A) — all 4 CX_ERR_ROUTER_*
// identities, their attributes (category / retryable / retry_after_ms /
// structured_data keys), the AgentFriendlyError builder, and the Status bridge.
namespace cortrix::query {
namespace {

using agent_friendly::ErrorCategory;

// Every enum value the suite walks (kept in sync with kRouterErrorCodeCount).
constexpr RouterErrorCode kAll[] = {
    RouterErrorCode::kClassifierLoadFailed,
    RouterErrorCode::kInferenceFailed,
    RouterErrorCode::kForceRouteInvalid,
    RouterErrorCode::kFallbackTriggered,
};

TEST(RouterErrorTest, CountMatchesEnumeration) {
    EXPECT_EQ(kRouterErrorCodeCount, 4);
    EXPECT_EQ(std::size(kAll), static_cast<size_t>(kRouterErrorCodeCount));
}

TEST(RouterErrorTest, AllCodesHaveUniqueRouterCxStrings) {
    std::set<std::string> seen;
    for (RouterErrorCode c : kAll) {
        std::string s = RouterErrorCodeString(c);
        EXPECT_FALSE(s.empty());
        EXPECT_EQ(s.rfind("CX_ERR_ROUTER_", 0), 0u) << s << " must start with CX_ERR_ROUTER_";
        EXPECT_TRUE(seen.insert(s).second) << "duplicate code string: " << s;
    }
    EXPECT_EQ(seen.size(), 4u);
}

// table, row by row.
TEST(RouterErrorTest, RegistryMatchesSpecTable) {
    auto chk = [](RouterErrorCode c, const char* code, ErrorCategory cat,
                  bool retry, std::optional<int> retry_ms) {
        const RouterErrorInfo& i = GetRouterErrorInfo(c);
        EXPECT_STREQ(i.cx_code, code);
        EXPECT_EQ(i.category, cat) << code;
        EXPECT_EQ(i.retryable, retry) << code;
        EXPECT_EQ(i.retry_after_ms, retry_ms) << code;
    };
    chk(RouterErrorCode::kClassifierLoadFailed, "CX_ERR_ROUTER_CLASSIFIER_LOAD_FAILED",
        ErrorCategory::kPermanent, false, std::nullopt);
    chk(RouterErrorCode::kInferenceFailed, "CX_ERR_ROUTER_INFERENCE_FAILED",
        ErrorCategory::kTransient, true, 100);
    chk(RouterErrorCode::kForceRouteInvalid, "CX_ERR_ROUTER_FORCE_ROUTE_INVALID",
        ErrorCategory::kPermanent, false, std::nullopt);
    // lists FALLBACK_TRIGGERED as transient + retryable with retry_after_ms "-"
    // (unspecified) → null. It is informational (the transparent Complex degrade
    // already happened), so no concrete back-off is advertised.
    chk(RouterErrorCode::kFallbackTriggered, "CX_ERR_ROUTER_FALLBACK_TRIGGERED",
        ErrorCategory::kTransient, true, std::nullopt);
}

TEST(RouterErrorTest, NonRetryablesCarryNoRetryHint) {
    EXPECT_FALSE(GetRouterErrorInfo(RouterErrorCode::kClassifierLoadFailed)
                     .retry_after_ms.has_value());
    EXPECT_FALSE(GetRouterErrorInfo(RouterErrorCode::kForceRouteInvalid)
                     .retry_after_ms.has_value());
    // INFERENCE_FAILED is retryable AND advertises a positive back-off (= 100).
    const auto& inf = GetRouterErrorInfo(RouterErrorCode::kInferenceFailed);
    ASSERT_TRUE(inf.retry_after_ms.has_value());
    EXPECT_EQ(*inf.retry_after_ms, 100);
}

TEST(RouterErrorTest, RequiredStructuredDataKeysPerSpec) {
    EXPECT_EQ(RequiredStructuredDataKeys(RouterErrorCode::kClassifierLoadFailed),
              (std::vector<std::string>{"model_path", "version"}));
    EXPECT_EQ(RequiredStructuredDataKeys(RouterErrorCode::kInferenceFailed),
              (std::vector<std::string>{"query_preview", "retry_count"}));
    EXPECT_EQ(RequiredStructuredDataKeys(RouterErrorCode::kForceRouteInvalid),
              (std::vector<std::string>{"invalid_route_value"}));
    EXPECT_EQ(RequiredStructuredDataKeys(RouterErrorCode::kFallbackTriggered),
              (std::vector<std::string>{"fallback_reason"}));
}

TEST(RouterErrorTest, HasRequiredStructuredDataValidatesKeys) {
    nlohmann::json full = {{"query_preview", "What is Q3 revenue..."}, {"retry_count", 3}};
    EXPECT_TRUE(HasRequiredStructuredData(RouterErrorCode::kInferenceFailed, full));

    nlohmann::json missing = {{"query_preview", "..."}};
    EXPECT_FALSE(HasRequiredStructuredData(RouterErrorCode::kInferenceFailed, missing));

    // Non-object payload never satisfies a code that requires keys.
    EXPECT_FALSE(HasRequiredStructuredData(RouterErrorCode::kForceRouteInvalid,
                                           nlohmann::json("not-an-object")));
}

TEST(RouterErrorTest, EveryCodeRequiresAtLeastOneStructuredKey) {
    for (RouterErrorCode c : kAll) {
        EXPECT_FALSE(RequiredStructuredDataKeys(c).empty()) << RouterErrorCodeString(c);
    }
}

TEST(RouterErrorTest, MakeRouterErrorFillsFromRegistry) {
    nlohmann::json sd = {{"query_preview", "compare X vs Y"}, {"retry_count", 3}};
    auto err = MakeRouterError(RouterErrorCode::kInferenceFailed, sd,
                               "Query router classifier inference failed, defaulting to complex");
    EXPECT_EQ(err.code, "CX_ERR_ROUTER_INFERENCE_FAILED");
    EXPECT_EQ(err.message,
              "Query router classifier inference failed, defaulting to complex");
    EXPECT_TRUE(err.retryable);
    EXPECT_EQ(err.category, ErrorCategory::kTransient);
    ASSERT_TRUE(err.retry_after_ms.has_value());
    EXPECT_EQ(*err.retry_after_ms, 100);
    ASSERT_TRUE(err.structured_data.has_value());
    EXPECT_EQ((*err.structured_data)["retry_count"], 3);
}

TEST(RouterErrorTest, MakeRouterErrorDefaultsMessageToCode) {
    auto err = MakeRouterError(RouterErrorCode::kClassifierLoadFailed);
    EXPECT_EQ(err.message, "CX_ERR_ROUTER_CLASSIFIER_LOAD_FAILED");
    EXPECT_FALSE(err.retryable);
    EXPECT_FALSE(err.retry_after_ms.has_value());
}

TEST(RouterErrorTest, ToJsonSerializesAgentFriendlyBody) {
    auto err = MakeRouterError(RouterErrorCode::kInferenceFailed,
                               {{"query_preview", "..."}, {"retry_count", 3}});
    nlohmann::json j = agent_friendly::ToJson(err);
    EXPECT_EQ(j["code"], "CX_ERR_ROUTER_INFERENCE_FAILED");
    EXPECT_EQ(j["retryable"], true);
    EXPECT_EQ(j["category"], "transient");
    EXPECT_EQ(j["retry_after_ms"], 100);
    EXPECT_EQ(j["structured_data"]["retry_count"], 3);
}

TEST(RouterErrorTest, StatusBridgeCarriesCodeToken) {
    Status s = RouterStatus(RouterErrorCode::kForceRouteInvalid, "banana");
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kInvalidArgument);
    EXPECT_NE(s.message().find("CX_ERR_ROUTER_FORCE_ROUTE_INVALID"), std::string::npos);
    EXPECT_NE(s.message().find("banana"), std::string::npos);
}

TEST(RouterErrorTest, StatusBridgeDefaultsDetailToCode) {
    Status s = RouterStatus(RouterErrorCode::kInferenceFailed);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.message(), "CX_ERR_ROUTER_INFERENCE_FAILED");
}

TEST(RouterErrorTest, StatusCodeMappingIsTotalAndSane) {
    for (RouterErrorCode c : kAll) {
        StatusCode sc = RouterErrorToStatusCode(c);
        EXPECT_NE(sc, StatusCode::kOk) << RouterErrorCodeString(c);
    }
    EXPECT_EQ(RouterErrorToStatusCode(RouterErrorCode::kForceRouteInvalid),
              StatusCode::kInvalidArgument);
    EXPECT_EQ(RouterErrorToStatusCode(RouterErrorCode::kClassifierLoadFailed),
              StatusCode::kInternal);
    EXPECT_EQ(RouterErrorToStatusCode(RouterErrorCode::kInferenceFailed),
              StatusCode::kUnavailable);
    EXPECT_EQ(RouterErrorToStatusCode(RouterErrorCode::kFallbackTriggered),
              StatusCode::kUnavailable);
}

}  // namespace
}  // namespace cortrix::query
