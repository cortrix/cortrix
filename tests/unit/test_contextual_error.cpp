#include <gtest/gtest.h>

#include <set>
#include <string>

#include "cortrix/agent_friendly/error.h"
#include "cortrix/spc/contextual_error.h"

// Contextual retrieval S7 — Contextual Retrieval error model (ARCH) — template A
// registry. Pins the 5 codes, categories/retryability/retry_after_ms,
// structured_data contracts, and the Status bridge.
namespace cortrix::spc {
namespace {

using agent_friendly::ErrorCategory;

const ContextualErrorCode kAll[] = {
    ContextualErrorCode::kLlmFailed,       ContextualErrorCode::kBudgetExceeded,
    ContextualErrorCode::kPromptInjection, ContextualErrorCode::kEmbeddingFailed,
    ContextualErrorCode::kStartupNoLlm,
};

TEST(ContextualErrorTest, CountIsFive) {
    EXPECT_EQ(kContextualErrorCodeCount, 5);
    EXPECT_EQ(std::size(kAll), 5u);
}

TEST(ContextualErrorTest, CodeStringsMatchDesign) {
    EXPECT_STREQ(ContextualErrorCodeString(ContextualErrorCode::kLlmFailed),
                 "CX_ERR_CONTEXTUAL_LLM_FAILED");
    EXPECT_STREQ(ContextualErrorCodeString(ContextualErrorCode::kBudgetExceeded),
                 "CX_ERR_CONTEXTUAL_BUDGET_EXCEEDED");
    EXPECT_STREQ(ContextualErrorCodeString(ContextualErrorCode::kPromptInjection),
                 "CX_ERR_CONTEXTUAL_PROMPT_INJECTION");
    EXPECT_STREQ(ContextualErrorCodeString(ContextualErrorCode::kEmbeddingFailed),
                 "CX_ERR_CONTEXTUAL_EMBEDDING_FAILED");
    EXPECT_STREQ(ContextualErrorCodeString(ContextualErrorCode::kStartupNoLlm),
                 "CX_ERR_CONTEXTUAL_STARTUP_NO_LLM");
}

TEST(ContextualErrorTest, AllCodesUniqueAndPrefixed) {
    std::set<std::string> seen;
    for (auto code : kAll) {
        std::string s = ContextualErrorCodeString(code);
        EXPECT_EQ(s.rfind("CX_ERR_CONTEXTUAL_", 0), 0u) << s;
        EXPECT_TRUE(seen.insert(s).second) << "duplicate " << s;
    }
    EXPECT_EQ(seen.size(), 5u);
}

TEST(ContextualErrorTest, CategoryAndRetryMatchDesign) {
    auto chk = [](ContextualErrorCode c, ErrorCategory cat, bool retry,
                  std::optional<int> ms) {
        const auto& info = GetContextualErrorInfo(c);
        EXPECT_EQ(info.category, cat);
        EXPECT_EQ(info.retryable, retry);
        EXPECT_EQ(info.retry_after_ms, ms);
    };
    chk(ContextualErrorCode::kLlmFailed, ErrorCategory::kTransient, true, 1000);
    chk(ContextualErrorCode::kBudgetExceeded, ErrorCategory::kQuota, false,
        std::nullopt);
    chk(ContextualErrorCode::kPromptInjection, ErrorCategory::kPermanent, false,
        std::nullopt);
    chk(ContextualErrorCode::kEmbeddingFailed, ErrorCategory::kTransient, true, 500);
    chk(ContextualErrorCode::kStartupNoLlm, ErrorCategory::kPermanent, false,
        std::nullopt);
}

TEST(ContextualErrorTest, RetryableImpliesRetryAfterSet) {
    for (auto code : kAll) {
        const auto& info = GetContextualErrorInfo(code);
        if (info.retryable) {
            EXPECT_TRUE(info.retry_after_ms.has_value())
                << ContextualErrorCodeString(code);
        } else {
            EXPECT_FALSE(info.retry_after_ms.has_value());
        }
    }
}

TEST(ContextualErrorTest, RequiredStructuredDataKeys) {
    EXPECT_EQ(RequiredStructuredDataKeys(ContextualErrorCode::kLlmFailed),
              (std::vector<std::string>{"chunk_id", "retry_count"}));
    EXPECT_EQ(RequiredStructuredDataKeys(ContextualErrorCode::kBudgetExceeded),
              (std::vector<std::string>{"ns_id", "monthly_quota", "used"}));
    EXPECT_EQ(RequiredStructuredDataKeys(ContextualErrorCode::kPromptInjection),
              (std::vector<std::string>{"chunk_id", "output_length"}));
    EXPECT_EQ(RequiredStructuredDataKeys(ContextualErrorCode::kStartupNoLlm),
              (std::vector<std::string>{"enricher_chain"}));
}

TEST(ContextualErrorTest, RequiredStructuredDataKeysEmbeddingAndStartup) {
    EXPECT_EQ(RequiredStructuredDataKeys(ContextualErrorCode::kEmbeddingFailed),
              (std::vector<std::string>{"chunk_id"}));
    EXPECT_EQ(RequiredStructuredDataKeys(ContextualErrorCode::kStartupNoLlm),
              (std::vector<std::string>{"enricher_chain"}));
}

TEST(ContextualErrorTest, HasRequiredStructuredData) {
    nlohmann::json full = {{"chunk_id", "c1"}, {"output_length", 500}};
    EXPECT_TRUE(HasRequiredStructuredData(ContextualErrorCode::kPromptInjection, full));
    nlohmann::json missing = {{"chunk_id", "c1"}};
    EXPECT_FALSE(
        HasRequiredStructuredData(ContextualErrorCode::kPromptInjection, missing));
}

TEST(ContextualErrorTest, HasRequiredStructuredDataNonObjectIsFalse) {
    // A non-object structured_data fails the contract for codes that require keys
    // (covers the !is_object() branch — required-keys are non-empty here).
    nlohmann::json not_object = nlohmann::json::array({1, 2, 3});
    EXPECT_FALSE(
        HasRequiredStructuredData(ContextualErrorCode::kLlmFailed, not_object));
}

TEST(ContextualErrorTest, MakeContextualErrorFillsFromRegistry) {
    auto err = MakeContextualError(ContextualErrorCode::kLlmFailed,
                                   {{"chunk_id", "c1"}, {"retry_count", 3}});
    EXPECT_EQ(err.code, "CX_ERR_CONTEXTUAL_LLM_FAILED");
    EXPECT_TRUE(err.retryable);
    EXPECT_EQ(err.category, ErrorCategory::kTransient);
    ASSERT_TRUE(err.retry_after_ms.has_value());
    EXPECT_EQ(*err.retry_after_ms, 1000);
}

TEST(ContextualErrorTest, MakeContextualErrorSerializesToAgentFriendlyBody) {
    auto err = MakeContextualError(ContextualErrorCode::kPromptInjection,
                                   {{"chunk_id", "c1"}, {"output_length", 999}});
    auto j = agent_friendly::ToJson(err);
    EXPECT_EQ(j["code"], "CX_ERR_CONTEXTUAL_PROMPT_INJECTION");
    EXPECT_EQ(j["retryable"], false);
    EXPECT_EQ(j["category"], "permanent");
    EXPECT_TRUE(j["retry_after_ms"].is_null());
    EXPECT_EQ(j["structured_data"]["output_length"], 999);
}

TEST(ContextualErrorTest, MakeContextualErrorDefaultMessageIsCode) {
    auto err = MakeContextualError(ContextualErrorCode::kEmbeddingFailed);
    EXPECT_EQ(err.message, "CX_ERR_CONTEXTUAL_EMBEDDING_FAILED");
}

TEST(ContextualErrorTest, StatusBridgeCarriesTokenAndCode) {
    Status s = ContextualStatus(ContextualErrorCode::kLlmFailed, "connect refused");
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kUnavailable);
    EXPECT_NE(s.message().find("CX_ERR_CONTEXTUAL_LLM_FAILED"), std::string::npos);
    EXPECT_NE(s.message().find("connect refused"), std::string::npos);
}

TEST(ContextualErrorTest, StatusCodeMapping) {
    EXPECT_EQ(ContextualErrorToStatusCode(ContextualErrorCode::kPromptInjection),
              StatusCode::kInvalidArgument);
    EXPECT_EQ(ContextualErrorToStatusCode(ContextualErrorCode::kBudgetExceeded),
              StatusCode::kPermissionDenied);
    EXPECT_EQ(ContextualErrorToStatusCode(ContextualErrorCode::kLlmFailed),
              StatusCode::kUnavailable);
    EXPECT_EQ(ContextualErrorToStatusCode(ContextualErrorCode::kEmbeddingFailed),
              StatusCode::kUnavailable);
    EXPECT_EQ(ContextualErrorToStatusCode(ContextualErrorCode::kStartupNoLlm),
              StatusCode::kUnavailable);
}

}  // namespace
}  // namespace cortrix::spc
