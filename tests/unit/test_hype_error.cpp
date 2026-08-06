#include <gtest/gtest.h>

#include <set>
#include <string>

#include "cortrix/agent_friendly/error.h"
#include "cortrix/spc/hype_error.h"

// HyPE S5 — HyPE error model (ARCH) — template A registry. Pins the 6
// codes, categories/retryability/retry_after_ms, structured_data contracts, and
// the Status bridge.
namespace cortrix::spc {
namespace {

using agent_friendly::ErrorCategory;

const HypeErrorCode kAll[] = {
    HypeErrorCode::kLlmTimeout,          HypeErrorCode::kLlmInvalidOutput,
    HypeErrorCode::kLlmBudgetExceeded,   HypeErrorCode::kQuestionParseFailed,
    HypeErrorCode::kSchemaVersionMismatch, HypeErrorCode::kParentNotFound,
};

TEST(HypeErrorTest, CountIsSix) {
    EXPECT_EQ(kHypeErrorCodeCount, 6);
    EXPECT_EQ(std::size(kAll), 6u);
}

TEST(HypeErrorTest, CodeStringsMatchDesign) {
    EXPECT_STREQ(HypeErrorCodeString(HypeErrorCode::kLlmTimeout),
                 "CX_ERR_HYPE_LLM_TIMEOUT");
    EXPECT_STREQ(HypeErrorCodeString(HypeErrorCode::kLlmInvalidOutput),
                 "CX_ERR_HYPE_LLM_INVALID_OUTPUT");
    EXPECT_STREQ(HypeErrorCodeString(HypeErrorCode::kLlmBudgetExceeded),
                 "CX_ERR_HYPE_LLM_BUDGET_EXCEEDED");
    EXPECT_STREQ(HypeErrorCodeString(HypeErrorCode::kQuestionParseFailed),
                 "CX_ERR_HYPE_QUESTION_PARSE_FAILED");
    EXPECT_STREQ(HypeErrorCodeString(HypeErrorCode::kSchemaVersionMismatch),
                 "CX_ERR_HYPE_SCHEMA_VERSION_MISMATCH");
    EXPECT_STREQ(HypeErrorCodeString(HypeErrorCode::kParentNotFound),
                 "CX_ERR_HYPE_PARENT_NOT_FOUND");
}

TEST(HypeErrorTest, AllCodesUniqueAndPrefixed) {
    std::set<std::string> seen;
    for (auto code : kAll) {
        std::string s = HypeErrorCodeString(code);
        EXPECT_EQ(s.rfind("CX_ERR_HYPE_", 0), 0u) << s;
        EXPECT_TRUE(seen.insert(s).second) << "duplicate " << s;
    }
    EXPECT_EQ(seen.size(), 6u);
}

TEST(HypeErrorTest, CategoryAndRetryMatchDesign) {
    auto chk = [](HypeErrorCode c, ErrorCategory cat, bool retry,
                  std::optional<int> ms) {
        const auto& info = GetHypeErrorInfo(c);
        EXPECT_EQ(info.category, cat);
        EXPECT_EQ(info.retryable, retry);
        EXPECT_EQ(info.retry_after_ms, ms);
    };
    chk(HypeErrorCode::kLlmTimeout, ErrorCategory::kTimeout, true, 5000);
    chk(HypeErrorCode::kLlmInvalidOutput, ErrorCategory::kTransient, true, 200);
    chk(HypeErrorCode::kLlmBudgetExceeded, ErrorCategory::kQuota, true, 60000);
    chk(HypeErrorCode::kQuestionParseFailed, ErrorCategory::kTransient, true, 200);
    chk(HypeErrorCode::kSchemaVersionMismatch, ErrorCategory::kPermanent, false,
        std::nullopt);
    chk(HypeErrorCode::kParentNotFound, ErrorCategory::kPermanent, false,
        std::nullopt);
}

TEST(HypeErrorTest, RetryableImpliesRetryAfterSet) {
    for (auto code : kAll) {
        const auto& info = GetHypeErrorInfo(code);
        if (info.retryable) {
            EXPECT_TRUE(info.retry_after_ms.has_value()) << HypeErrorCodeString(code);
        } else {
            EXPECT_FALSE(info.retry_after_ms.has_value());
        }
    }
}

TEST(HypeErrorTest, RequiredStructuredDataKeys) {
    EXPECT_EQ(RequiredStructuredDataKeys(HypeErrorCode::kLlmTimeout),
              (std::vector<std::string>{"chunk_id", "attempt_count",
                                        "last_error_message"}));
    EXPECT_EQ(RequiredStructuredDataKeys(HypeErrorCode::kQuestionParseFailed),
              (std::vector<std::string>{"chunk_id", "expected_count",
                                        "actual_count"}));
    EXPECT_EQ(RequiredStructuredDataKeys(HypeErrorCode::kSchemaVersionMismatch),
              (std::vector<std::string>{"expected_version", "actual_version"}));
    EXPECT_EQ(RequiredStructuredDataKeys(HypeErrorCode::kParentNotFound),
              (std::vector<std::string>{"parent_id", "store_error"}));
}

TEST(HypeErrorTest, HasRequiredStructuredData) {
    nlohmann::json full = {{"parent_id", "p1"}, {"store_error", "CX_ERR_STORE_NOT_FOUND"}};
    EXPECT_TRUE(HasRequiredStructuredData(HypeErrorCode::kParentNotFound, full));
    nlohmann::json missing = {{"parent_id", "p1"}};
    EXPECT_FALSE(HasRequiredStructuredData(HypeErrorCode::kParentNotFound, missing));
}

TEST(HypeErrorTest, MakeHypeErrorFillsFromRegistry) {
    auto err = MakeHypeError(
        HypeErrorCode::kLlmBudgetExceeded,
        {{"chunk_id", "c1"}, {"budget_remaining_usd", 0.0}, {"budget_reset_at", "t"}});
    EXPECT_EQ(err.code, "CX_ERR_HYPE_LLM_BUDGET_EXCEEDED");
    EXPECT_TRUE(err.retryable);
    EXPECT_EQ(err.category, ErrorCategory::kQuota);
    ASSERT_TRUE(err.retry_after_ms.has_value());
    EXPECT_EQ(*err.retry_after_ms, 60000);
}

TEST(HypeErrorTest, MakeHypeErrorSerializesToAgentFriendlyBody) {
    auto err = MakeHypeError(HypeErrorCode::kQuestionParseFailed,
                             {{"chunk_id", "c1"}, {"expected_count", 3},
                              {"actual_count", 2}});
    auto j = agent_friendly::ToJson(err);
    EXPECT_EQ(j["code"], "CX_ERR_HYPE_QUESTION_PARSE_FAILED");
    EXPECT_EQ(j["retryable"], true);
    EXPECT_EQ(j["category"], "transient");
    EXPECT_EQ(j["retry_after_ms"], 200);
    EXPECT_EQ(j["structured_data"]["expected_count"], 3);
}

TEST(HypeErrorTest, MakeHypeErrorDefaultMessageIsCode) {
    auto err = MakeHypeError(HypeErrorCode::kLlmTimeout);
    EXPECT_EQ(err.message, "CX_ERR_HYPE_LLM_TIMEOUT");
}

TEST(HypeErrorTest, StatusBridgeCarriesTokenAndCode) {
    Status s = HypeStatus(HypeErrorCode::kParentNotFound, "missing p1");
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kNotFound);
    EXPECT_NE(s.message().find("CX_ERR_HYPE_PARENT_NOT_FOUND"), std::string::npos);
    EXPECT_NE(s.message().find("missing p1"), std::string::npos);
}

TEST(HypeErrorTest, StatusCodeMapping) {
    EXPECT_EQ(HypeErrorToStatusCode(HypeErrorCode::kSchemaVersionMismatch),
              StatusCode::kInvalidArgument);
    EXPECT_EQ(HypeErrorToStatusCode(HypeErrorCode::kParentNotFound),
              StatusCode::kNotFound);
    EXPECT_EQ(HypeErrorToStatusCode(HypeErrorCode::kLlmTimeout),
              StatusCode::kUnavailable);
    EXPECT_EQ(HypeErrorToStatusCode(HypeErrorCode::kQuestionParseFailed),
              StatusCode::kUnavailable);
}

}  // namespace
}  // namespace cortrix::spc
