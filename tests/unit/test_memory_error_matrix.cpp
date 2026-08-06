#include <gtest/gtest.h>

#include <regex>
#include <set>
#include <string>
#include <vector>

#include "cortrix/agent_friendly/error.h"
#include "cortrix/memory/memory_error.h"

// Exhaustive parameterized error-registry sweep for memory transparency. Distinct
// suite name (MemoryErrorMatrix) from the basic test_memory_error.cpp.
namespace cortrix::memory::transparency {
namespace {

using agent_friendly::ErrorCategory;

const MemoryErrorCode kAll[] = {
    MemoryErrorCode::kMemoryNotFound,
    MemoryErrorCode::kUserMismatch,
    MemoryErrorCode::kAlreadyInvalidated,
    MemoryErrorCode::kInvalidateFailed,
    MemoryErrorCode::kQuota,
};

bool IsValidCategory(ErrorCategory c) {
    return c == ErrorCategory::kAuth || c == ErrorCategory::kQuota ||
           c == ErrorCategory::kTransient || c == ErrorCategory::kPermanent ||
           c == ErrorCategory::kTimeout;
}

class MemoryErrorMatrix : public ::testing::TestWithParam<MemoryErrorCode> {};

TEST_P(MemoryErrorMatrix, CodeStringFormat) {
    const MemoryErrorCode code = GetParam();
    const std::string s = MemoryErrorCodeString(code);
    EXPECT_FALSE(s.empty());
    EXPECT_EQ(s.rfind("CX_ERR_MEMORY_", 0), 0u) << s;
    EXPECT_TRUE(std::regex_match(s, std::regex("^CX_ERR_MEMORY_[A-Z0-9_]+$"))) << s;
    EXPECT_STREQ(GetMemoryErrorInfo(code).cx_code, s.c_str());
}

TEST_P(MemoryErrorMatrix, CategoryAndRetryInvariant) {
    const MemoryErrorCode code = GetParam();
    const MemoryErrorInfo& info = GetMemoryErrorInfo(code);
    EXPECT_TRUE(IsValidCategory(info.category));
    if (!info.retryable) {
        EXPECT_FALSE(info.retry_after_ms.has_value()) << info.cx_code;
    }
    if (info.retry_after_ms.has_value()) {
        EXPECT_GT(*info.retry_after_ms, 0) << info.cx_code;
    }
    EXPECT_GT(MemoryErrorHttpStatus(code), 0);
}

TEST_P(MemoryErrorMatrix, RequiredStructuredDataArms) {
    const MemoryErrorCode code = GetParam();
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

TEST_P(MemoryErrorMatrix, MakeRoundTripAndJson) {
    const MemoryErrorCode code = GetParam();
    const MemoryErrorInfo& info = GetMemoryErrorInfo(code);

    nlohmann::json sd = nlohmann::json::object();
    for (const std::string& k : RequiredStructuredDataKeys(code)) sd[k] = "v";

    auto err = MakeMemoryError(code, sd, "human detail");
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

TEST_P(MemoryErrorMatrix, EmptyMessageFallsBackToCode) {
    const MemoryErrorCode code = GetParam();
    auto err = MakeMemoryError(code);
    EXPECT_EQ(err.message, std::string(MemoryErrorCodeString(code)));
}

TEST_P(MemoryErrorMatrix, StatusBridge) {
    const MemoryErrorCode code = GetParam();
    const StatusCode mapped = MemoryErrorToStatusCode(code);
    EXPECT_NE(mapped, StatusCode::kOk);

    Status s = MemoryStatus(code, "detail-x");
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), mapped);
    EXPECT_NE(s.message().find(MemoryErrorCodeString(code)), std::string::npos);
    EXPECT_NE(s.message().find("detail-x"), std::string::npos);
}

TEST(MemoryErrorMatrixAggregate, CountAndUniqueCodes) {
    EXPECT_EQ(kMemoryErrorCodeCount, 5);
    EXPECT_EQ(std::size(kAll), static_cast<size_t>(kMemoryErrorCodeCount));
    std::set<std::string> seen;
    for (MemoryErrorCode c : kAll) {
        EXPECT_TRUE(seen.insert(MemoryErrorCodeString(c)).second);
    }
    EXPECT_EQ(seen.size(), static_cast<size_t>(kMemoryErrorCodeCount));
}

INSTANTIATE_TEST_SUITE_P(AllCodes, MemoryErrorMatrix, ::testing::ValuesIn(kAll));

}  // namespace
}  // namespace cortrix::memory::transparency
