#include <gtest/gtest.h>

#include <regex>
#include <set>
#include <string>
#include <vector>

#include "cortrix/agent_friendly/error.h"
#include "cortrix/memory/mem04_error.h"

// Exhaustive parameterized error-registry sweep for MEM04 (ARCH §4.1.11, 7 codes).
// Distinct suite name (Mem04ErrorMatrix) from the basic test_mem04_error.cpp.
namespace cortrix::memory::immunity {
namespace {

using agent_friendly::ErrorCategory;

const Mem04ErrorCode kAll[] = {
    Mem04ErrorCode::kSessionNotFound,
    Mem04ErrorCode::kAlreadyOptedOut,
    Mem04ErrorCode::kNotOptedOut,
    Mem04ErrorCode::kRevokeDenied,
    Mem04ErrorCode::kOptOutDisabled,
    Mem04ErrorCode::kInvalidSessionId,
    Mem04ErrorCode::kMetadataTooLarge,
};

bool IsValidCategory(ErrorCategory c) {
    return c == ErrorCategory::kAuth || c == ErrorCategory::kQuota ||
           c == ErrorCategory::kTransient || c == ErrorCategory::kPermanent ||
           c == ErrorCategory::kTimeout;
}

class Mem04ErrorMatrix : public ::testing::TestWithParam<Mem04ErrorCode> {};

TEST_P(Mem04ErrorMatrix, CodeStringFormat) {
    const Mem04ErrorCode code = GetParam();
    const std::string s = Mem04ErrorCodeString(code);
    EXPECT_FALSE(s.empty());
    EXPECT_EQ(s.rfind("CX_ERR_MEM04_", 0), 0u) << s;
    EXPECT_TRUE(std::regex_match(s, std::regex("^CX_ERR_MEM04_[A-Z0-9_]+$"))) << s;
    EXPECT_STREQ(GetMem04ErrorInfo(code).cx_code, s.c_str());
}

TEST_P(Mem04ErrorMatrix, CategoryAndRetryInvariant) {
    const Mem04ErrorCode code = GetParam();
    const Mem04ErrorInfo& info = GetMem04ErrorInfo(code);
    EXPECT_TRUE(IsValidCategory(info.category));
    // All 7 MEM04 codes are non-retryable per ARCH §4.1.11.
    EXPECT_FALSE(info.retryable) << info.cx_code;
    EXPECT_FALSE(info.retry_after_ms.has_value()) << info.cx_code;
    EXPECT_GT(Mem04ErrorHttpStatus(code), 0);
}

TEST_P(Mem04ErrorMatrix, RequiredStructuredDataArms) {
    const Mem04ErrorCode code = GetParam();
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

TEST_P(Mem04ErrorMatrix, MakeRoundTripAndJson) {
    const Mem04ErrorCode code = GetParam();
    const Mem04ErrorInfo& info = GetMem04ErrorInfo(code);

    nlohmann::json sd = nlohmann::json::object();
    for (const std::string& k : RequiredStructuredDataKeys(code)) sd[k] = "v";

    auto err = MakeMem04Error(code, sd, "human detail");
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

TEST_P(Mem04ErrorMatrix, EmptyMessageFallsBackToCode) {
    const Mem04ErrorCode code = GetParam();
    auto err = MakeMem04Error(code);
    EXPECT_EQ(err.message, std::string(Mem04ErrorCodeString(code)));
}

TEST_P(Mem04ErrorMatrix, StatusBridge) {
    const Mem04ErrorCode code = GetParam();
    const StatusCode mapped = Mem04ErrorToStatusCode(code);
    EXPECT_NE(mapped, StatusCode::kOk);

    Status s = Mem04Status(code, "detail-x");
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), mapped);
    EXPECT_NE(s.message().find(Mem04ErrorCodeString(code)), std::string::npos);
    EXPECT_NE(s.message().find("detail-x"), std::string::npos);
}

TEST(Mem04ErrorMatrixAggregate, CountAndUniqueCodes) {
    EXPECT_EQ(kMem04ErrorCodeCount, 7);
    EXPECT_EQ(std::size(kAll), static_cast<size_t>(kMem04ErrorCodeCount));
    std::set<std::string> seen;
    for (Mem04ErrorCode c : kAll) {
        EXPECT_TRUE(seen.insert(Mem04ErrorCodeString(c)).second);
    }
    EXPECT_EQ(seen.size(), static_cast<size_t>(kMem04ErrorCodeCount));
}

INSTANTIATE_TEST_SUITE_P(AllCodes, Mem04ErrorMatrix, ::testing::ValuesIn(kAll));

}  // namespace
}  // namespace cortrix::memory::immunity
