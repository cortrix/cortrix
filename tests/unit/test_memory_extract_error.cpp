#include <gtest/gtest.h>

#include <set>
#include <string>

#include "cortrix/memory/memory_extract_error.h"

// S6 coverage: the memory extraction error model (template A) — all 5 CX_ERR_MEMEXTRACT_* identities,
// their attributes (http/category/retryable/retry_after_ms/structured_data
// keys), the AgentFriendlyError builder, and the Status bridge.
namespace cortrix::memory {
namespace {

using agent_friendly::ErrorCategory;

// Every enum value the suite walks (kept in sync with kMemoryExtractErrorCodeCount).
constexpr MemoryExtractErrorCode kAll[] = {
    MemoryExtractErrorCode::kExtractLlmTimeout,
    MemoryExtractErrorCode::kExtractInvalidOutput,
    MemoryExtractErrorCode::kExtractBudgetExceeded,
    MemoryExtractErrorCode::kContradictionAmbiguous,
    MemoryExtractErrorCode::kLlmDisabled,
};

TEST(MemoryExtractErrorTest, CountMatchesEnumeration) {
    EXPECT_EQ(kMemoryExtractErrorCodeCount, 5);
    EXPECT_EQ(std::size(kAll), static_cast<size_t>(kMemoryExtractErrorCodeCount));
}

TEST(MemoryExtractErrorTest, AllCodesHaveUniqueCxStrings) {
    std::set<std::string> seen;
    for (MemoryExtractErrorCode c : kAll) {
        std::string s = MemoryExtractErrorCodeString(c);
        EXPECT_FALSE(s.empty());
        EXPECT_EQ(s.rfind("CX_ERR_MEMEXTRACT_", 0), 0u) << s << " must start with CX_ERR_MEMEXTRACT_";
        EXPECT_TRUE(seen.insert(s).second) << "duplicate code string: " << s;
    }
    EXPECT_EQ(seen.size(), 5u);
}

// table, row by row.
TEST(MemoryExtractErrorTest, RegistryMatchesSpecTable) {
    auto chk = [](MemoryExtractErrorCode c, const char* code, int http, ErrorCategory cat,
                  bool retry, std::optional<int> retry_ms) {
        const MemoryExtractErrorInfo& i = GetMemoryExtractErrorInfo(c);
        EXPECT_STREQ(i.cx_code, code);
        EXPECT_EQ(i.http_status, http) << code;
        EXPECT_EQ(i.category, cat) << code;
        EXPECT_EQ(i.retryable, retry) << code;
        EXPECT_EQ(i.retry_after_ms, retry_ms) << code;
    };
    chk(MemoryExtractErrorCode::kExtractLlmTimeout, "CX_ERR_MEMEXTRACT_LLM_TIMEOUT", 504,
        ErrorCategory::kTimeout, true, 5000);
    chk(MemoryExtractErrorCode::kExtractInvalidOutput, "CX_ERR_MEMEXTRACT_INVALID_OUTPUT", 500,
        ErrorCategory::kTransient, true, 5000);
    chk(MemoryExtractErrorCode::kExtractBudgetExceeded, "CX_ERR_MEMEXTRACT_BUDGET_EXCEEDED", 429,
        ErrorCategory::kQuota, false, std::nullopt);
    chk(MemoryExtractErrorCode::kContradictionAmbiguous, "CX_ERR_MEMEXTRACT_CONTRADICTION_AMBIGUOUS", 500,
        ErrorCategory::kTransient, true, 5000);
    chk(MemoryExtractErrorCode::kLlmDisabled, "CX_ERR_MEMEXTRACT_LLM_DISABLED", 503,
        ErrorCategory::kPermanent, false, std::nullopt);
}

TEST(MemoryExtractErrorTest, RetryableImpliesRetryAfterMs) {
    // GEN-Agent #6: a retryable error carries a machine-readable retry hint; a
    // non-retryable one carries none.
    for (MemoryExtractErrorCode c : kAll) {
        const MemoryExtractErrorInfo& i = GetMemoryExtractErrorInfo(c);
        if (i.retryable) {
            EXPECT_TRUE(i.retry_after_ms.has_value()) << i.cx_code;
            EXPECT_GT(*i.retry_after_ms, 0) << i.cx_code;
        } else {
            EXPECT_FALSE(i.retry_after_ms.has_value()) << i.cx_code;
        }
    }
}

TEST(MemoryExtractErrorTest, HttpStatusAccessor) {
    EXPECT_EQ(MemoryExtractErrorHttpStatus(MemoryExtractErrorCode::kExtractLlmTimeout), 504);
    EXPECT_EQ(MemoryExtractErrorHttpStatus(MemoryExtractErrorCode::kExtractBudgetExceeded), 429);
    EXPECT_EQ(MemoryExtractErrorHttpStatus(MemoryExtractErrorCode::kLlmDisabled), 503);
}

TEST(MemoryExtractErrorTest, RequiredStructuredDataKeysPerSpec) {
    EXPECT_EQ(RequiredStructuredDataKeys(MemoryExtractErrorCode::kExtractLlmTimeout),
              (std::vector<std::string>{"interaction_id", "llm_model", "timeout_ms"}));
    EXPECT_EQ(RequiredStructuredDataKeys(MemoryExtractErrorCode::kExtractBudgetExceeded),
              (std::vector<std::string>{"budget_cap_usd", "current_usage_usd"}));
    EXPECT_EQ(RequiredStructuredDataKeys(MemoryExtractErrorCode::kExtractInvalidOutput),
              (std::vector<std::string>{"interaction_id", "llm_model"}));
    EXPECT_EQ(RequiredStructuredDataKeys(MemoryExtractErrorCode::kContradictionAmbiguous),
              (std::vector<std::string>{"new_block_id", "old_block_id", "confidence"}));
    EXPECT_EQ(RequiredStructuredDataKeys(MemoryExtractErrorCode::kLlmDisabled),
              (std::vector<std::string>{"reason"}));
}

TEST(MemoryExtractErrorTest, HasRequiredStructuredDataValidatesKeys) {
    nlohmann::json full = {{"interaction_id", "i1"}, {"llm_model", "gpt-4o-mini"},
                           {"timeout_ms", 30000}};
    EXPECT_TRUE(HasRequiredStructuredData(MemoryExtractErrorCode::kExtractLlmTimeout, full));

    nlohmann::json missing = {{"interaction_id", "i1"}};
    EXPECT_FALSE(HasRequiredStructuredData(MemoryExtractErrorCode::kExtractLlmTimeout, missing));

    // Non-object payload only passes when no keys are required (all 5 require keys).
    EXPECT_FALSE(HasRequiredStructuredData(MemoryExtractErrorCode::kLlmDisabled,
                                           nlohmann::json("not-an-object")));
}

TEST(MemoryExtractErrorTest, MakeMemoryExtractErrorFillsFromRegistry) {
    nlohmann::json sd = {{"interaction_id", "i1"}, {"llm_model", "gpt-4o-mini"},
                         {"timeout_ms", 30000}};
    auto err = MakeMemoryExtractError(MemoryExtractErrorCode::kExtractLlmTimeout, sd,
                              "LLM extraction timed out");
    EXPECT_EQ(err.code, "CX_ERR_MEMEXTRACT_LLM_TIMEOUT");
    EXPECT_EQ(err.message, "LLM extraction timed out");
    EXPECT_TRUE(err.retryable);
    EXPECT_EQ(err.category, ErrorCategory::kTimeout);
    ASSERT_TRUE(err.retry_after_ms.has_value());
    EXPECT_EQ(*err.retry_after_ms, 5000);
    ASSERT_TRUE(err.structured_data.has_value());
    EXPECT_EQ((*err.structured_data)["interaction_id"], "i1");
}

TEST(MemoryExtractErrorTest, MakeMemoryExtractErrorDefaultsMessageToCode) {
    auto err = MakeMemoryExtractError(MemoryExtractErrorCode::kLlmDisabled);
    EXPECT_EQ(err.message, "CX_ERR_MEMEXTRACT_LLM_DISABLED");
    EXPECT_FALSE(err.retryable);
    EXPECT_FALSE(err.retry_after_ms.has_value());
}

TEST(MemoryExtractErrorTest, ToJsonSerializesAgentFriendlyBody) {
    auto err = MakeMemoryExtractError(MemoryExtractErrorCode::kExtractBudgetExceeded,
                              {{"budget_cap_usd", 10}, {"current_usage_usd", 12}});
    nlohmann::json j = agent_friendly::ToJson(err);
    EXPECT_EQ(j["code"], "CX_ERR_MEMEXTRACT_BUDGET_EXCEEDED");
    EXPECT_EQ(j["retryable"], false);
    EXPECT_EQ(j["category"], "quota");
    EXPECT_TRUE(j["retry_after_ms"].is_null());
    EXPECT_EQ(j["structured_data"]["budget_cap_usd"], 10);
}

TEST(MemoryExtractErrorTest, StatusBridgeCarriesCodeToken) {
    Status s = MemoryExtractStatus(MemoryExtractErrorCode::kExtractBudgetExceeded, "daily cap hit");
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kPermissionDenied);
    EXPECT_NE(s.message().find("CX_ERR_MEMEXTRACT_BUDGET_EXCEEDED"), std::string::npos);
    EXPECT_NE(s.message().find("daily cap hit"), std::string::npos);
}

TEST(MemoryExtractErrorTest, StatusCodeMappingIsTotalAndSane) {
    for (MemoryExtractErrorCode c : kAll) {
        StatusCode sc = MemoryExtractErrorToStatusCode(c);
        EXPECT_NE(sc, StatusCode::kOk) << MemoryExtractErrorCodeString(c);
    }
    EXPECT_EQ(MemoryExtractErrorToStatusCode(MemoryExtractErrorCode::kExtractLlmTimeout),
              StatusCode::kUnavailable);
    EXPECT_EQ(MemoryExtractErrorToStatusCode(MemoryExtractErrorCode::kExtractInvalidOutput),
              StatusCode::kInternal);
    EXPECT_EQ(MemoryExtractErrorToStatusCode(MemoryExtractErrorCode::kExtractBudgetExceeded),
              StatusCode::kPermissionDenied);
    EXPECT_EQ(MemoryExtractErrorToStatusCode(MemoryExtractErrorCode::kContradictionAmbiguous),
              StatusCode::kInternal);
    EXPECT_EQ(MemoryExtractErrorToStatusCode(MemoryExtractErrorCode::kLlmDisabled),
              StatusCode::kUnavailable);
}

}  // namespace
}  // namespace cortrix::memory
