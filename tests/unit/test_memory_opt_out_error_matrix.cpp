#include <gtest/gtest.h>

#include <regex>
#include <set>
#include <string>
#include <vector>

#include "cortrix/agent_friendly/error.h"
#include "cortrix/memory/memory_opt_out_error.h"

// Exhaustive parameterized error-registry sweep for memory opt-out (ARCH §4.1.11, 7 codes).
// Distinct suite name (MemoryOptOutErrorMatrix) from the basic test_memory_opt_out_error.cpp.
namespace cortrix::memory::immunity {
namespace {

using agent_friendly::ErrorCategory;

const MemoryOptOutErrorCode kAll[] = {
    MemoryOptOutErrorCode::kSessionNotFound,
    MemoryOptOutErrorCode::kAlreadyOptedOut,
    MemoryOptOutErrorCode::kNotOptedOut,
    MemoryOptOutErrorCode::kRevokeDenied,
    MemoryOptOutErrorCode::kOptOutDisabled,
    MemoryOptOutErrorCode::kInvalidSessionId,
    MemoryOptOutErrorCode::kMetadataTooLarge,
};

bool IsValidCategory(ErrorCategory c) {
    return c == ErrorCategory::kAuth || c == ErrorCategory::kQuota ||
           c == ErrorCategory::kTransient || c == ErrorCategory::kPermanent ||
           c == ErrorCategory::kTimeout;
}

class MemoryOptOutErrorMatrix : public ::testing::TestWithParam<MemoryOptOutErrorCode> {};

TEST_P(MemoryOptOutErrorMatrix, CodeStringFormat) {
    const MemoryOptOutErrorCode code = GetParam();
    const std::string s = MemoryOptOutErrorCodeString(code);
    EXPECT_FALSE(s.empty());
    EXPECT_EQ(s.rfind("CX_ERR_MEMOPTOUT_", 0), 0u) << s;
    EXPECT_TRUE(std::regex_match(s, std::regex("^CX_ERR_MEMOPTOUT_[A-Z0-9_]+$"))) << s;
    EXPECT_STREQ(GetMemoryOptOutErrorInfo(code).cx_code, s.c_str());
}

TEST_P(MemoryOptOutErrorMatrix, CategoryAndRetryInvariant) {
    const MemoryOptOutErrorCode code = GetParam();
    const MemoryOptOutErrorInfo& info = GetMemoryOptOutErrorInfo(code);
    EXPECT_TRUE(IsValidCategory(info.category));
    // All 7 memory opt-out codes are non-retryable per ARCH §4.1.11.
    EXPECT_FALSE(info.retryable) << info.cx_code;
    EXPECT_FALSE(info.retry_after_ms.has_value()) << info.cx_code;
    EXPECT_GT(MemoryOptOutErrorHttpStatus(code), 0);
}

TEST_P(MemoryOptOutErrorMatrix, RequiredStructuredDataArms) {
    const MemoryOptOutErrorCode code = GetParam();
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

TEST_P(MemoryOptOutErrorMatrix, MakeRoundTripAndJson) {
    const MemoryOptOutErrorCode code = GetParam();
    const MemoryOptOutErrorInfo& info = GetMemoryOptOutErrorInfo(code);

    nlohmann::json sd = nlohmann::json::object();
    for (const std::string& k : RequiredStructuredDataKeys(code)) sd[k] = "v";

    auto err = MakeMemoryOptOutError(code, sd, "human detail");
    EXPECT_EQ(err.code, info.cx_code);
    EXPECT_EQ(err.message, "human detail");
    EXPECT_EQ(err.retryable, info.retryable);
    EXPECT_EQ(err.category, info.category);
    EXPECT_EQ(err.retry_after_ms, info.retry_after_ms);

    nlohmann::json j = agent_friendly::ToJson(err);
    EXPECT_EQ(j["code"], std::string(info.cx_code));
    EXPECT_EQ(j["retryable"], info.retryable);
    EXPECT_EQ(j["category"], std::string(agent_friendly::ToString(info.category)));
    EXPECT_TRUE(j["retry_after_ms"].is_null());
}

TEST_P(MemoryOptOutErrorMatrix, EmptyMessageFallsBackToCode) {
    const MemoryOptOutErrorCode code = GetParam();
    auto err = MakeMemoryOptOutError(code);
    EXPECT_EQ(err.message, std::string(MemoryOptOutErrorCodeString(code)));
}

TEST_P(MemoryOptOutErrorMatrix, StatusBridge) {
    const MemoryOptOutErrorCode code = GetParam();
    const StatusCode mapped = MemoryOptOutErrorToStatusCode(code);
    EXPECT_NE(mapped, StatusCode::kOk);

    Status s = MemoryOptOutStatus(code, "detail-x");
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), mapped);
    EXPECT_NE(s.message().find(MemoryOptOutErrorCodeString(code)), std::string::npos);
    EXPECT_NE(s.message().find("detail-x"), std::string::npos);
}

TEST(MemoryOptOutErrorMatrixAggregate, CountAndUniqueCodes) {
    EXPECT_EQ(kMemoryOptOutErrorCodeCount, 7);
    EXPECT_EQ(std::size(kAll), static_cast<size_t>(kMemoryOptOutErrorCodeCount));
    std::set<std::string> seen;
    for (MemoryOptOutErrorCode c : kAll) {
        EXPECT_TRUE(seen.insert(MemoryOptOutErrorCodeString(c)).second);
    }
    EXPECT_EQ(seen.size(), static_cast<size_t>(kMemoryOptOutErrorCodeCount));
}

INSTANTIATE_TEST_SUITE_P(AllCodes, MemoryOptOutErrorMatrix, ::testing::ValuesIn(kAll));

}  // namespace
}  // namespace cortrix::memory::immunity
