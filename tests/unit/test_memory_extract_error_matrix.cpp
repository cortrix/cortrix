#include <gtest/gtest.h>

#include <regex>
#include <set>
#include <string>
#include <vector>

#include "cortrix/agent_friendly/error.h"
#include "cortrix/memory/memory_extract_error.h"

// Exhaustive parameterized error-registry sweep for memory extraction. Distinct suite
// name (MemoryExtractErrorMatrix) from the basic test_memory_extract_error.cpp (MemoryExtractErrorTest) so
// the two coexist. Walks EVERY MemoryExtractErrorCode asserting the registry invariants:
// code-string format/uniqueness, category set membership, retryable<=>retry_after,
// RequiredStructuredDataKeys + HasRequiredStructuredData arms, Make round-trip +
// JSON + empty-message fallback, and the ToStatusCode / Status bridge.
namespace cortrix::memory {
namespace {

using agent_friendly::ErrorCategory;

const MemoryExtractErrorCode kAll[] = {
    MemoryExtractErrorCode::kExtractLlmTimeout,
    MemoryExtractErrorCode::kExtractInvalidOutput,
    MemoryExtractErrorCode::kExtractBudgetExceeded,
    MemoryExtractErrorCode::kContradictionAmbiguous,
    MemoryExtractErrorCode::kLlmDisabled,
};

bool IsValidCategory(ErrorCategory c) {
    return c == ErrorCategory::kAuth || c == ErrorCategory::kQuota ||
           c == ErrorCategory::kTransient || c == ErrorCategory::kPermanent ||
           c == ErrorCategory::kTimeout;
}

class MemoryExtractErrorMatrix : public ::testing::TestWithParam<MemoryExtractErrorCode> {};

TEST_P(MemoryExtractErrorMatrix, CodeStringFormat) {
    const MemoryExtractErrorCode code = GetParam();
    const std::string s = MemoryExtractErrorCodeString(code);
    EXPECT_FALSE(s.empty());
    EXPECT_EQ(s.rfind("CX_ERR_MEMEXTRACT_", 0), 0u) << s;
    EXPECT_TRUE(std::regex_match(s, std::regex("^CX_ERR_MEMEXTRACT_[A-Z0-9_]+$"))) << s;
    EXPECT_STREQ(GetMemoryExtractErrorInfo(code).cx_code, s.c_str());
}

TEST_P(MemoryExtractErrorMatrix, CategoryAndRetryInvariant) {
    const MemoryExtractErrorCode code = GetParam();
    const MemoryExtractErrorInfo& info = GetMemoryExtractErrorInfo(code);
    EXPECT_TRUE(IsValidCategory(info.category));
    if (!info.retryable) {
        EXPECT_FALSE(info.retry_after_ms.has_value()) << info.cx_code;
    }
    if (info.retry_after_ms.has_value()) {
        EXPECT_GT(*info.retry_after_ms, 0) << info.cx_code;
    }
    EXPECT_GT(MemoryExtractErrorHttpStatus(code), 0);
}

TEST_P(MemoryExtractErrorMatrix, RequiredStructuredDataArms) {
    const MemoryExtractErrorCode code = GetParam();
    const std::vector<std::string>& keys = RequiredStructuredDataKeys(code);

    // All-present object passes.
    nlohmann::json full = nlohmann::json::object();
    for (const std::string& k : keys) full[k] = "v";
    EXPECT_TRUE(HasRequiredStructuredData(code, full));

    // Missing one key fails (only meaningful when keys exist).
    if (!keys.empty()) {
        nlohmann::json missing = full;
        missing.erase(keys.front());
        EXPECT_FALSE(HasRequiredStructuredData(code, missing));
    }

    // Non-object only passes when no keys are required.
    EXPECT_EQ(HasRequiredStructuredData(code, nlohmann::json("not-an-object")),
              keys.empty());
}

TEST_P(MemoryExtractErrorMatrix, MakeRoundTripAndJson) {
    const MemoryExtractErrorCode code = GetParam();
    const MemoryExtractErrorInfo& info = GetMemoryExtractErrorInfo(code);

    nlohmann::json sd = nlohmann::json::object();
    for (const std::string& k : RequiredStructuredDataKeys(code)) sd[k] = "v";

    auto err = MakeMemoryExtractError(code, sd, "human detail");
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

TEST_P(MemoryExtractErrorMatrix, EmptyMessageFallsBackToCode) {
    const MemoryExtractErrorCode code = GetParam();
    auto err = MakeMemoryExtractError(code);
    EXPECT_EQ(err.message, std::string(MemoryExtractErrorCodeString(code)));
}

TEST_P(MemoryExtractErrorMatrix, StatusBridge) {
    const MemoryExtractErrorCode code = GetParam();
    const StatusCode mapped = MemoryExtractErrorToStatusCode(code);
    EXPECT_NE(mapped, StatusCode::kOk);

    Status s = MemoryExtractStatus(code, "detail-x");
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), mapped);
    EXPECT_NE(s.message().find(MemoryExtractErrorCodeString(code)), std::string::npos);
    EXPECT_NE(s.message().find("detail-x"), std::string::npos);
}

TEST(MemoryExtractErrorMatrixAggregate, CountAndUniqueCodes) {
    EXPECT_EQ(kMemoryExtractErrorCodeCount, 5);
    EXPECT_EQ(std::size(kAll), static_cast<size_t>(kMemoryExtractErrorCodeCount));
    std::set<std::string> seen;
    for (MemoryExtractErrorCode c : kAll) {
        EXPECT_TRUE(seen.insert(MemoryExtractErrorCodeString(c)).second);
    }
    EXPECT_EQ(seen.size(), static_cast<size_t>(kMemoryExtractErrorCodeCount));
}

INSTANTIATE_TEST_SUITE_P(AllCodes, MemoryExtractErrorMatrix, ::testing::ValuesIn(kAll));

}  // namespace
}  // namespace cortrix::memory
