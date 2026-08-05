#include <gtest/gtest.h>

#include <set>
#include <string>

#include "cortrix/memory/mem02_error.h"

// S6 coverage: the memory extraction error model (template A) — all 5 CX_ERR_MEM02_* identities,
// their §5.3 attributes (http/category/retryable/retry_after_ms/structured_data
// keys), the AgentFriendlyError builder, and the Status bridge.
namespace cortrix::memory {
namespace {

using agent_friendly::ErrorCategory;

// Every enum value the suite walks (kept in sync with kMem02ErrorCodeCount).
constexpr Mem02ErrorCode kAll[] = {
    Mem02ErrorCode::kExtractLlmTimeout,
    Mem02ErrorCode::kExtractInvalidOutput,
    Mem02ErrorCode::kExtractBudgetExceeded,
    Mem02ErrorCode::kContradictionAmbiguous,
    Mem02ErrorCode::kLlmDisabled,
};

TEST(Mem02ErrorTest, CountMatchesEnumeration) {
    EXPECT_EQ(kMem02ErrorCodeCount, 5);
    EXPECT_EQ(std::size(kAll), static_cast<size_t>(kMem02ErrorCodeCount));
}

TEST(Mem02ErrorTest, AllCodesHaveUniqueCxStrings) {
    std::set<std::string> seen;
    for (Mem02ErrorCode c : kAll) {
        std::string s = Mem02ErrorCodeString(c);
        EXPECT_FALSE(s.empty());
        EXPECT_EQ(s.rfind("CX_ERR_MEM02_", 0), 0u) << s << " must start with CX_ERR_MEM02_";
        EXPECT_TRUE(seen.insert(s).second) << "duplicate code string: " << s;
    }
    EXPECT_EQ(seen.size(), 5u);
}

// §5.3 table, row by row.
TEST(Mem02ErrorTest, RegistryMatchesSpecTable) {
    auto chk = [](Mem02ErrorCode c, const char* code, int http, ErrorCategory cat,
                  bool retry, std::optional<int> retry_ms) {
        const Mem02ErrorInfo& i = GetMem02ErrorInfo(c);
        EXPECT_STREQ(i.cx_code, code);
        EXPECT_EQ(i.http_status, http) << code;
        EXPECT_EQ(i.category, cat) << code;
        EXPECT_EQ(i.retryable, retry) << code;
        EXPECT_EQ(i.retry_after_ms, retry_ms) << code;
    };
    chk(Mem02ErrorCode::kExtractLlmTimeout, "CX_ERR_MEM02_EXTRACT_LLM_TIMEOUT", 504,
        ErrorCategory::kTimeout, true, 5000);
    chk(Mem02ErrorCode::kExtractInvalidOutput, "CX_ERR_MEM02_EXTRACT_INVALID_OUTPUT", 500,
        ErrorCategory::kTransient, true, 5000);
    chk(Mem02ErrorCode::kExtractBudgetExceeded, "CX_ERR_MEM02_EXTRACT_BUDGET_EXCEEDED", 429,
        ErrorCategory::kQuota, false, std::nullopt);
    chk(Mem02ErrorCode::kContradictionAmbiguous, "CX_ERR_MEM02_CONTRADICTION_AMBIGUOUS", 500,
        ErrorCategory::kTransient, true, 5000);
    chk(Mem02ErrorCode::kLlmDisabled, "CX_ERR_MEM02_LLM_DISABLED", 503,
        ErrorCategory::kPermanent, false, std::nullopt);
}

TEST(Mem02ErrorTest, RetryableImpliesRetryAfterMs) {
    // GEN-Agent #6: a retryable error carries a machine-readable retry hint; a
    // non-retryable one carries none.
    for (Mem02ErrorCode c : kAll) {
        const Mem02ErrorInfo& i = GetMem02ErrorInfo(c);
        if (i.retryable) {
            EXPECT_TRUE(i.retry_after_ms.has_value()) << i.cx_code;
            EXPECT_GT(*i.retry_after_ms, 0) << i.cx_code;
        } else {
            EXPECT_FALSE(i.retry_after_ms.has_value()) << i.cx_code;
        }
    }
}

TEST(Mem02ErrorTest, HttpStatusAccessor) {
    EXPECT_EQ(Mem02ErrorHttpStatus(Mem02ErrorCode::kExtractLlmTimeout), 504);
    EXPECT_EQ(Mem02ErrorHttpStatus(Mem02ErrorCode::kExtractBudgetExceeded), 429);
    EXPECT_EQ(Mem02ErrorHttpStatus(Mem02ErrorCode::kLlmDisabled), 503);
}

TEST(Mem02ErrorTest, RequiredStructuredDataKeysPerSpec) {
    EXPECT_EQ(RequiredStructuredDataKeys(Mem02ErrorCode::kExtractLlmTimeout),
              (std::vector<std::string>{"interaction_id", "llm_model", "timeout_ms"}));
    EXPECT_EQ(RequiredStructuredDataKeys(Mem02ErrorCode::kExtractBudgetExceeded),
              (std::vector<std::string>{"budget_cap_usd", "current_usage_usd"}));
    EXPECT_EQ(RequiredStructuredDataKeys(Mem02ErrorCode::kExtractInvalidOutput),
              (std::vector<std::string>{"interaction_id", "llm_model"}));
    EXPECT_EQ(RequiredStructuredDataKeys(Mem02ErrorCode::kContradictionAmbiguous),
              (std::vector<std::string>{"new_block_id", "old_block_id", "confidence"}));
    EXPECT_EQ(RequiredStructuredDataKeys(Mem02ErrorCode::kLlmDisabled),
              (std::vector<std::string>{"reason"}));
}

TEST(Mem02ErrorTest, HasRequiredStructuredDataValidatesKeys) {
    nlohmann::json full = {{"interaction_id", "i1"}, {"llm_model", "gpt-4o-mini"},
                           {"timeout_ms", 30000}};
    EXPECT_TRUE(HasRequiredStructuredData(Mem02ErrorCode::kExtractLlmTimeout, full));

    nlohmann::json missing = {{"interaction_id", "i1"}};
    EXPECT_FALSE(HasRequiredStructuredData(Mem02ErrorCode::kExtractLlmTimeout, missing));

    // Non-object payload only passes when no keys are required (all 5 require keys).
    EXPECT_FALSE(HasRequiredStructuredData(Mem02ErrorCode::kLlmDisabled,
                                           nlohmann::json("not-an-object")));
}

TEST(Mem02ErrorTest, MakeMem02ErrorFillsFromRegistry) {
    nlohmann::json sd = {{"interaction_id", "i1"}, {"llm_model", "gpt-4o-mini"},
                         {"timeout_ms", 30000}};
    auto err = MakeMem02Error(Mem02ErrorCode::kExtractLlmTimeout, sd,
                              "LLM extraction timed out");
    EXPECT_EQ(err.code, "CX_ERR_MEM02_EXTRACT_LLM_TIMEOUT");
    EXPECT_EQ(err.message, "LLM extraction timed out");
    EXPECT_TRUE(err.retryable);
    EXPECT_EQ(err.category, ErrorCategory::kTimeout);
    ASSERT_TRUE(err.retry_after_ms.has_value());
    EXPECT_EQ(*err.retry_after_ms, 5000);
    ASSERT_TRUE(err.structured_data.has_value());
    EXPECT_EQ((*err.structured_data)["interaction_id"], "i1");
}

TEST(Mem02ErrorTest, MakeMem02ErrorDefaultsMessageToCode) {
    auto err = MakeMem02Error(Mem02ErrorCode::kLlmDisabled);
    EXPECT_EQ(err.message, "CX_ERR_MEM02_LLM_DISABLED");
    EXPECT_FALSE(err.retryable);
    EXPECT_FALSE(err.retry_after_ms.has_value());
}

TEST(Mem02ErrorTest, ToJsonSerializesAgentFriendlyBody) {
    auto err = MakeMem02Error(Mem02ErrorCode::kExtractBudgetExceeded,
                              {{"budget_cap_usd", 10}, {"current_usage_usd", 12}});
    nlohmann::json j = agent_friendly::ToJson(err);
    EXPECT_EQ(j["code"], "CX_ERR_MEM02_EXTRACT_BUDGET_EXCEEDED");
    EXPECT_EQ(j["retryable"], false);
    EXPECT_EQ(j["category"], "quota");
    EXPECT_TRUE(j["retry_after_ms"].is_null());
    EXPECT_EQ(j["structured_data"]["budget_cap_usd"], 10);
}

TEST(Mem02ErrorTest, StatusBridgeCarriesCodeToken) {
    Status s = Mem02Status(Mem02ErrorCode::kExtractBudgetExceeded, "daily cap hit");
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kPermissionDenied);
    EXPECT_NE(s.message().find("CX_ERR_MEM02_EXTRACT_BUDGET_EXCEEDED"), std::string::npos);
    EXPECT_NE(s.message().find("daily cap hit"), std::string::npos);
}

TEST(Mem02ErrorTest, StatusCodeMappingIsTotalAndSane) {
    for (Mem02ErrorCode c : kAll) {
        StatusCode sc = Mem02ErrorToStatusCode(c);
        EXPECT_NE(sc, StatusCode::kOk) << Mem02ErrorCodeString(c);
    }
    EXPECT_EQ(Mem02ErrorToStatusCode(Mem02ErrorCode::kExtractLlmTimeout),
              StatusCode::kUnavailable);
    EXPECT_EQ(Mem02ErrorToStatusCode(Mem02ErrorCode::kExtractInvalidOutput),
              StatusCode::kInternal);
    EXPECT_EQ(Mem02ErrorToStatusCode(Mem02ErrorCode::kExtractBudgetExceeded),
              StatusCode::kPermissionDenied);
    EXPECT_EQ(Mem02ErrorToStatusCode(Mem02ErrorCode::kContradictionAmbiguous),
              StatusCode::kInternal);
    EXPECT_EQ(Mem02ErrorToStatusCode(Mem02ErrorCode::kLlmDisabled),
              StatusCode::kUnavailable);
}

}  // namespace
}  // namespace cortrix::memory
