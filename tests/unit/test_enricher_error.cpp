#include "cortrix/spc_enricher/enricher_error.h"

#include <gtest/gtest.h>

#include "cortrix/agent_friendly/error.h"
#include "cortrix/common/status.h"

namespace cortrix::spc {
namespace {

using agent_friendly::ErrorCategory;

// All 6 codes for table-driven coverage.
constexpr EnricherErrorCode kAllCodes[] = {
    EnricherErrorCode::kInitFailed, EnricherErrorCode::kLlmTimeout,
    EnricherErrorCode::kLlmApi,     EnricherErrorCode::kParse,
    EnricherErrorCode::kBudget,     EnricherErrorCode::kRateLimit,
};

TEST(EnricherErrorTest, CountAnchorMatchesEnum) {
    // GEN-Agent #7 compat anchor — the set must not shrink.
    EXPECT_EQ(kEnricherErrorCodeCount, 6);
    EXPECT_EQ(std::size(kAllCodes), static_cast<size_t>(kEnricherErrorCodeCount));
}

TEST(EnricherErrorTest, EveryCodeHasStableCxString) {
    for (auto code : kAllCodes) {
        const char* s = EnricherErrorCodeString(code);
        ASSERT_NE(s, nullptr);
        EXPECT_EQ(std::string(s).rfind("CX_ERR_ENRICHER_", 0), 0u)
            << "code string must start with CX_ERR_ENRICHER_: " << s;
    }
}

TEST(EnricherErrorTest, RegistryMatchesDesignMatrix) {
    // §5.1 matrix — retryable / category / retry_after_ms per code.
    struct Row {
        EnricherErrorCode code;
        const char* cx;
        bool retryable;
        ErrorCategory category;
        bool has_retry_after;
        int retry_after_ms;
    };
    const Row rows[] = {
        {EnricherErrorCode::kInitFailed, "CX_ERR_ENRICHER_INIT_FAILED", false,
         ErrorCategory::kPermanent, false, 0},
        {EnricherErrorCode::kLlmTimeout, "CX_ERR_ENRICHER_LLM_TIMEOUT", true,
         ErrorCategory::kTimeout, true, 1000},
        {EnricherErrorCode::kLlmApi, "CX_ERR_ENRICHER_LLM_API", true,
         ErrorCategory::kTransient, true, 2000},
        {EnricherErrorCode::kParse, "CX_ERR_ENRICHER_PARSE", false,
         ErrorCategory::kPermanent, false, 0},
        {EnricherErrorCode::kBudget, "CX_ERR_ENRICHER_BUDGET", false,
         ErrorCategory::kQuota, false, 0},
        {EnricherErrorCode::kRateLimit, "CX_ERR_ENRICHER_RATE_LIMIT", true,
         ErrorCategory::kTransient, true, 60000},
    };
    for (const Row& r : rows) {
        const EnricherErrorInfo& info = GetEnricherErrorInfo(r.code);
        EXPECT_STREQ(info.cx_code, r.cx);
        EXPECT_EQ(info.retryable, r.retryable) << r.cx;
        EXPECT_EQ(info.category, r.category) << r.cx;
        EXPECT_EQ(info.retry_after_ms.has_value(), r.has_retry_after) << r.cx;
        if (r.has_retry_after) {
            EXPECT_EQ(info.retry_after_ms.value(), r.retry_after_ms) << r.cx;
        }
        // Non-retryable codes carry no retry_after (matrix "-1 = N/A").
        if (!r.retryable) EXPECT_FALSE(info.retry_after_ms.has_value()) << r.cx;
    }
}

TEST(EnricherErrorTest, RequiredStructuredDataKeysPerCode) {
    // Spot-check the §5.1 structured_data columns.
    EXPECT_EQ(RequiredStructuredDataKeys(EnricherErrorCode::kInitFailed),
              (std::vector<std::string>{"reason", "endpoint", "fallback"}));
    EXPECT_EQ(RequiredStructuredDataKeys(EnricherErrorCode::kBudget),
              (std::vector<std::string>{"budget_cap_usd", "current_cost_usd", "model"}));
    EXPECT_EQ(RequiredStructuredDataKeys(EnricherErrorCode::kRateLimit),
              (std::vector<std::string>{"model", "http_status", "retry_after_sec"}));
}

TEST(EnricherErrorTest, HasRequiredStructuredDataValidates) {
    nlohmann::json complete = {
        {"budget_cap_usd", 100}, {"current_cost_usd", 99.8}, {"model", "gpt-4o-mini"}};
    EXPECT_TRUE(HasRequiredStructuredData(EnricherErrorCode::kBudget, complete));

    nlohmann::json missing = {{"budget_cap_usd", 100}};
    EXPECT_FALSE(HasRequiredStructuredData(EnricherErrorCode::kBudget, missing));

    // Non-object → only valid if no keys required (none of ours are key-free).
    EXPECT_FALSE(HasRequiredStructuredData(EnricherErrorCode::kBudget, nlohmann::json("x")));
}

TEST(EnricherErrorTest, MakeEnricherErrorFillsFromRegistry) {
    nlohmann::json data = {
        {"model", "gpt-4o-mini"}, {"http_status", 429}, {"retry_after_sec", 60}};
    auto err = MakeEnricherError(EnricherErrorCode::kRateLimit, data,
                                 "OpenAI API rate limit exceeded");
    EXPECT_EQ(err.code, "CX_ERR_ENRICHER_RATE_LIMIT");
    EXPECT_EQ(err.message, "OpenAI API rate limit exceeded");
    EXPECT_TRUE(err.retryable);
    EXPECT_EQ(err.category, ErrorCategory::kTransient);
    ASSERT_TRUE(err.retry_after_ms.has_value());
    EXPECT_EQ(err.retry_after_ms.value(), 60000);
    ASSERT_TRUE(err.structured_data.has_value());
    EXPECT_EQ(err.structured_data->at("http_status"), 429);

    // Empty message defaults to the cx_code (so the body is never blank).
    auto err2 = MakeEnricherError(EnricherErrorCode::kInitFailed);
    EXPECT_EQ(err2.message, "CX_ERR_ENRICHER_INIT_FAILED");
    EXPECT_FALSE(err2.retryable);
    EXPECT_FALSE(err2.retry_after_ms.has_value());
}

TEST(EnricherErrorTest, MakeEnricherErrorJsonSerializesAgentFriendly) {
    // Mirrors design §2.5b error JSON schema.
    auto err = MakeEnricherError(
        EnricherErrorCode::kRateLimit,
        {{"model", "gpt-4o-mini"}, {"http_status", 429}, {"retry_after_sec", 60}},
        "OpenAI API rate limit exceeded");
    auto j = agent_friendly::ToJson(err);
    EXPECT_EQ(j["code"], "CX_ERR_ENRICHER_RATE_LIMIT");
    EXPECT_EQ(j["retryable"], true);
    EXPECT_EQ(j["category"], "transient");
    EXPECT_EQ(j["retry_after_ms"], 60000);
    EXPECT_EQ(j["structured_data"]["http_status"], 429);
}

TEST(EnricherErrorTest, StatusBridgeCarriesCodeToken) {
    Status s = EnricherStatus(EnricherErrorCode::kLlmTimeout, "deadline exceeded");
    EXPECT_EQ(s.code(), StatusCode::kUnavailable);
    EXPECT_EQ(s.message().rfind("CX_ERR_ENRICHER_LLM_TIMEOUT:", 0), 0u);

    // No detail → message is just the token.
    Status s2 = EnricherStatus(EnricherErrorCode::kBudget);
    EXPECT_EQ(s2.message(), "CX_ERR_ENRICHER_BUDGET");
}

TEST(EnricherErrorTest, ErrorToStatusCodeMapping) {
    EXPECT_EQ(EnricherErrorToStatusCode(EnricherErrorCode::kInitFailed),
              StatusCode::kInvalidArgument);
    EXPECT_EQ(EnricherErrorToStatusCode(EnricherErrorCode::kParse),
              StatusCode::kInternal);
    EXPECT_EQ(EnricherErrorToStatusCode(EnricherErrorCode::kBudget),
              StatusCode::kPermissionDenied);
    EXPECT_EQ(EnricherErrorToStatusCode(EnricherErrorCode::kLlmTimeout),
              StatusCode::kUnavailable);
    EXPECT_EQ(EnricherErrorToStatusCode(EnricherErrorCode::kLlmApi),
              StatusCode::kUnavailable);
    EXPECT_EQ(EnricherErrorToStatusCode(EnricherErrorCode::kRateLimit),
              StatusCode::kUnavailable);
}

}  // namespace
}  // namespace cortrix::spc
