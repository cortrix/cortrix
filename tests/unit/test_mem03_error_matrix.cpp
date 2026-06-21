#include <gtest/gtest.h>

#include <regex>
#include <set>
#include <string>
#include <vector>

#include "cortrix/agent_friendly/error.h"
#include "cortrix/memory/mem03_error.h"

// Exhaustive parameterized error-registry sweep for MEM03 (§4.3.4.bis). Distinct
// suite name (Mem03ErrorMatrix) from the basic test_mem03_error.cpp.
namespace cortrix::memory::transparency {
namespace {

using agent_friendly::ErrorCategory;

const Mem03ErrorCode kAll[] = {
    Mem03ErrorCode::kMemoryNotFound,
    Mem03ErrorCode::kUserMismatch,
    Mem03ErrorCode::kAlreadyInvalidated,
    Mem03ErrorCode::kInvalidateFailed,
    Mem03ErrorCode::kQuota,
};

bool IsValidCategory(ErrorCategory c) {
    return c == ErrorCategory::kAuth || c == ErrorCategory::kQuota ||
           c == ErrorCategory::kTransient || c == ErrorCategory::kPermanent ||
           c == ErrorCategory::kTimeout;
}

class Mem03ErrorMatrix : public ::testing::TestWithParam<Mem03ErrorCode> {};

TEST_P(Mem03ErrorMatrix, CodeStringFormat) {
    const Mem03ErrorCode code = GetParam();
    const std::string s = Mem03ErrorCodeString(code);
    EXPECT_FALSE(s.empty());
    EXPECT_EQ(s.rfind("CX_ERR_MEM03_", 0), 0u) << s;
    EXPECT_TRUE(std::regex_match(s, std::regex("^CX_ERR_MEM03_[A-Z0-9_]+$"))) << s;
    EXPECT_STREQ(GetMem03ErrorInfo(code).cx_code, s.c_str());
}

TEST_P(Mem03ErrorMatrix, CategoryAndRetryInvariant) {
    const Mem03ErrorCode code = GetParam();
    const Mem03ErrorInfo& info = GetMem03ErrorInfo(code);
    EXPECT_TRUE(IsValidCategory(info.category));
    if (!info.retryable) {
        EXPECT_FALSE(info.retry_after_ms.has_value()) << info.cx_code;
    }
    if (info.retry_after_ms.has_value()) {
        EXPECT_GT(*info.retry_after_ms, 0) << info.cx_code;
    }
    EXPECT_GT(Mem03ErrorHttpStatus(code), 0);
}

TEST_P(Mem03ErrorMatrix, RequiredStructuredDataArms) {
    const Mem03ErrorCode code = GetParam();
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

TEST_P(Mem03ErrorMatrix, MakeRoundTripAndJson) {
    const Mem03ErrorCode code = GetParam();
    const Mem03ErrorInfo& info = GetMem03ErrorInfo(code);

    nlohmann::json sd = nlohmann::json::object();
    for (const std::string& k : RequiredStructuredDataKeys(code)) sd[k] = "v";

    auto err = MakeMem03Error(code, sd, "human detail");
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

TEST_P(Mem03ErrorMatrix, EmptyMessageFallsBackToCode) {
    const Mem03ErrorCode code = GetParam();
    auto err = MakeMem03Error(code);
    EXPECT_EQ(err.message, std::string(Mem03ErrorCodeString(code)));
}

TEST_P(Mem03ErrorMatrix, StatusBridge) {
    const Mem03ErrorCode code = GetParam();
    const StatusCode mapped = Mem03ErrorToStatusCode(code);
    EXPECT_NE(mapped, StatusCode::kOk);

    Status s = Mem03Status(code, "detail-x");
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), mapped);
    EXPECT_NE(s.message().find(Mem03ErrorCodeString(code)), std::string::npos);
    EXPECT_NE(s.message().find("detail-x"), std::string::npos);
}

TEST(Mem03ErrorMatrixAggregate, CountAndUniqueCodes) {
    EXPECT_EQ(kMem03ErrorCodeCount, 5);
    EXPECT_EQ(std::size(kAll), static_cast<size_t>(kMem03ErrorCodeCount));
    std::set<std::string> seen;
    for (Mem03ErrorCode c : kAll) {
        EXPECT_TRUE(seen.insert(Mem03ErrorCodeString(c)).second);
    }
    EXPECT_EQ(seen.size(), static_cast<size_t>(kMem03ErrorCodeCount));
}

INSTANTIATE_TEST_SUITE_P(AllCodes, Mem03ErrorMatrix, ::testing::ValuesIn(kAll));

}  // namespace
}  // namespace cortrix::memory::transparency
