#include <gtest/gtest.h>

#include <set>
#include <string>

#include "cortrix/agent_friendly/error.h"
#include "cortrix/doc_summary/doc_summary_error.h"

// Doc summary S5 — Document Summary error model (§7, ARCH §4.1.11) — template A registry.
// Pins the 7 codes, categories/retryability/retry_after_ms, structured_data
// contracts, and the Status bridge.
namespace cortrix::doc_summary {
namespace {

using agent_friendly::ErrorCategory;

const DocSummaryErrorCode kAll[] = {
    DocSummaryErrorCode::kLlmTimeout,            DocSummaryErrorCode::kLlmInvalidOutput,
    DocSummaryErrorCode::kLlmBudgetExceeded,     DocSummaryErrorCode::kDocTooLarge,
    DocSummaryErrorCode::kSchemaVersionMismatch, DocSummaryErrorCode::kFallbackFailed,
    DocSummaryErrorCode::kFts5FallbackFailed,
};

TEST(F41DocSummaryErrorTest, CountIsSeven) {
    EXPECT_EQ(kDocSummaryErrorCodeCount, 7);
    EXPECT_EQ(std::size(kAll), 7u);
}

TEST(F41DocSummaryErrorTest, CodeStringsMatchDesign) {
    EXPECT_STREQ(DocSummaryErrorCodeString(DocSummaryErrorCode::kLlmTimeout),
                 "CX_ERR_DOCSUMMARY_LLM_TIMEOUT");
    EXPECT_STREQ(DocSummaryErrorCodeString(DocSummaryErrorCode::kLlmInvalidOutput),
                 "CX_ERR_DOCSUMMARY_LLM_INVALID_OUTPUT");
    EXPECT_STREQ(DocSummaryErrorCodeString(DocSummaryErrorCode::kLlmBudgetExceeded),
                 "CX_ERR_DOCSUMMARY_LLM_BUDGET_EXCEEDED");
    EXPECT_STREQ(DocSummaryErrorCodeString(DocSummaryErrorCode::kDocTooLarge),
                 "CX_ERR_DOCSUMMARY_TOO_LARGE");
    EXPECT_STREQ(DocSummaryErrorCodeString(DocSummaryErrorCode::kSchemaVersionMismatch),
                 "CX_ERR_DOCSUMMARY_SCHEMA_VERSION_MISMATCH");
    EXPECT_STREQ(DocSummaryErrorCodeString(DocSummaryErrorCode::kFallbackFailed),
                 "CX_ERR_DOCSUMMARY_FALLBACK_FAILED");
    EXPECT_STREQ(DocSummaryErrorCodeString(DocSummaryErrorCode::kFts5FallbackFailed),
                 "CX_ERR_DOCSUMMARY_FTS5_FALLBACK_FAILED");
}

TEST(F41DocSummaryErrorTest, AllCodesUniqueAndPrefixed) {
    std::set<std::string> seen;
    for (auto code : kAll) {
        std::string s = DocSummaryErrorCodeString(code);
        EXPECT_EQ(s.rfind("CX_ERR_DOCSUMMARY_", 0), 0u) << s;
        EXPECT_TRUE(seen.insert(s).second) << "duplicate " << s;
    }
    EXPECT_EQ(seen.size(), 7u);
}

TEST(F41DocSummaryErrorTest, CategoryAndRetryMatchDesign) {
    auto chk = [](DocSummaryErrorCode c, ErrorCategory cat, bool retry,
                  std::optional<int> ms) {
        const auto& info = GetDocSummaryErrorInfo(c);
        EXPECT_EQ(info.category, cat);
        EXPECT_EQ(info.retryable, retry);
        EXPECT_EQ(info.retry_after_ms, ms);
    };
    chk(DocSummaryErrorCode::kLlmTimeout, ErrorCategory::kTimeout, true, 5000);
    chk(DocSummaryErrorCode::kLlmInvalidOutput, ErrorCategory::kTransient, true, 200);
    chk(DocSummaryErrorCode::kLlmBudgetExceeded, ErrorCategory::kQuota, true, 60000);
    chk(DocSummaryErrorCode::kDocTooLarge, ErrorCategory::kPermanent, false, std::nullopt);
    chk(DocSummaryErrorCode::kSchemaVersionMismatch, ErrorCategory::kPermanent, false,
        std::nullopt);
    chk(DocSummaryErrorCode::kFallbackFailed, ErrorCategory::kTransient, true, 200);
    chk(DocSummaryErrorCode::kFts5FallbackFailed, ErrorCategory::kTransient, true, 200);
}

TEST(F41DocSummaryErrorTest, RetryableImpliesRetryAfterSet) {
    for (auto code : kAll) {
        const auto& info = GetDocSummaryErrorInfo(code);
        if (info.retryable) {
            EXPECT_TRUE(info.retry_after_ms.has_value())
                << DocSummaryErrorCodeString(code);
        } else {
            EXPECT_FALSE(info.retry_after_ms.has_value());
        }
    }
}

TEST(F41DocSummaryErrorTest, RequiredStructuredDataKeys) {
    EXPECT_EQ(RequiredStructuredDataKeys(DocSummaryErrorCode::kLlmTimeout),
              (std::vector<std::string>{"doc_id", "attempt_count", "last_error_message"}));
    EXPECT_EQ(RequiredStructuredDataKeys(DocSummaryErrorCode::kDocTooLarge),
              (std::vector<std::string>{"doc_id", "doc_size_bytes", "max_allowed_bytes"}));
    EXPECT_EQ(RequiredStructuredDataKeys(DocSummaryErrorCode::kSchemaVersionMismatch),
              (std::vector<std::string>{"expected_version", "actual_version"}));
    EXPECT_EQ(RequiredStructuredDataKeys(DocSummaryErrorCode::kFts5FallbackFailed),
              (std::vector<std::string>{"doc_id", "fts5_query", "error_message"}));
    EXPECT_EQ(RequiredStructuredDataKeys(DocSummaryErrorCode::kFallbackFailed),
              (std::vector<std::string>{"doc_id", "fallback_type", "error_message"}));
}

TEST(F41DocSummaryErrorTest, HasRequiredStructuredData) {
    nlohmann::json full = {{"doc_id", "d1"}, {"fts5_query", "ai"}, {"error_message", "x"}};
    EXPECT_TRUE(HasRequiredStructuredData(DocSummaryErrorCode::kFts5FallbackFailed, full));
    nlohmann::json missing = {{"doc_id", "d1"}};
    EXPECT_FALSE(
        HasRequiredStructuredData(DocSummaryErrorCode::kFts5FallbackFailed, missing));
    nlohmann::json not_object = nlohmann::json::array({1});
    EXPECT_FALSE(
        HasRequiredStructuredData(DocSummaryErrorCode::kLlmTimeout, not_object));
}

TEST(F41DocSummaryErrorTest, MakeErrorFillsFromRegistry) {
    auto err = MakeDocSummaryError(
        DocSummaryErrorCode::kLlmBudgetExceeded,
        {{"doc_id", "d1"}, {"budget_remaining_usd", 0.0}, {"budget_reset_at", "t"}});
    EXPECT_EQ(err.code, "CX_ERR_DOCSUMMARY_LLM_BUDGET_EXCEEDED");
    EXPECT_TRUE(err.retryable);
    EXPECT_EQ(err.category, ErrorCategory::kQuota);
    ASSERT_TRUE(err.retry_after_ms.has_value());
    EXPECT_EQ(*err.retry_after_ms, 60000);
}

TEST(F41DocSummaryErrorTest, MakeErrorSerializesToAgentFriendlyBody) {
    auto err = MakeDocSummaryError(DocSummaryErrorCode::kDocTooLarge,
                                   {{"doc_id", "d1"}, {"doc_size_bytes", 99},
                                    {"max_allowed_bytes", 10}});
    auto j = agent_friendly::ToJson(err);
    EXPECT_EQ(j["code"], "CX_ERR_DOCSUMMARY_TOO_LARGE");
    EXPECT_EQ(j["retryable"], false);
    EXPECT_EQ(j["category"], "permanent");
    EXPECT_TRUE(j["retry_after_ms"].is_null());
    EXPECT_EQ(j["structured_data"]["doc_size_bytes"], 99);
}

TEST(F41DocSummaryErrorTest, MakeErrorDefaultMessageIsCode) {
    auto err = MakeDocSummaryError(DocSummaryErrorCode::kFallbackFailed);
    EXPECT_EQ(err.message, "CX_ERR_DOCSUMMARY_FALLBACK_FAILED");
}

TEST(F41DocSummaryErrorTest, StatusBridgeCarriesTokenAndCode) {
    Status s = DocSummaryStatus(DocSummaryErrorCode::kLlmTimeout, "slow");
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kUnavailable);
    EXPECT_NE(s.message().find("CX_ERR_DOCSUMMARY_LLM_TIMEOUT"), std::string::npos);
    EXPECT_NE(s.message().find("slow"), std::string::npos);
}

TEST(F41DocSummaryErrorTest, StatusCodeMapping) {
    EXPECT_EQ(DocSummaryErrorToStatusCode(DocSummaryErrorCode::kDocTooLarge),
              StatusCode::kInvalidArgument);
    EXPECT_EQ(DocSummaryErrorToStatusCode(DocSummaryErrorCode::kSchemaVersionMismatch),
              StatusCode::kInvalidArgument);
    EXPECT_EQ(DocSummaryErrorToStatusCode(DocSummaryErrorCode::kLlmTimeout),
              StatusCode::kUnavailable);
    EXPECT_EQ(DocSummaryErrorToStatusCode(DocSummaryErrorCode::kFts5FallbackFailed),
              StatusCode::kUnavailable);
}

}  // namespace
}  // namespace cortrix::doc_summary
