#include <gtest/gtest.h>

#include <regex>
#include <set>
#include <string>
#include <vector>

#include "cortrix/agent_friendly/error.h"
#include "cortrix/memory/mem02_error.h"

// Exhaustive parameterized error-registry sweep for memory extraction (§5.3). Distinct suite
// name (Mem02ErrorMatrix) from the basic test_mem02_error.cpp (Mem02ErrorTest) so
// the two coexist. Walks EVERY Mem02ErrorCode asserting the registry invariants:
// code-string format/uniqueness, category set membership, retryable<=>retry_after,
// RequiredStructuredDataKeys + HasRequiredStructuredData arms, Make round-trip +
// JSON + empty-message fallback, and the ToStatusCode / Status bridge.
namespace cortrix::memory {
namespace {

using agent_friendly::ErrorCategory;

const Mem02ErrorCode kAll[] = {
    Mem02ErrorCode::kExtractLlmTimeout,
    Mem02ErrorCode::kExtractInvalidOutput,
    Mem02ErrorCode::kExtractBudgetExceeded,
    Mem02ErrorCode::kContradictionAmbiguous,
    Mem02ErrorCode::kLlmDisabled,
};

bool IsValidCategory(ErrorCategory c) {
    return c == ErrorCategory::kAuth || c == ErrorCategory::kQuota ||
           c == ErrorCategory::kTransient || c == ErrorCategory::kPermanent ||
           c == ErrorCategory::kTimeout;
}

class Mem02ErrorMatrix : public ::testing::TestWithParam<Mem02ErrorCode> {};

TEST_P(Mem02ErrorMatrix, CodeStringFormat) {
    const Mem02ErrorCode code = GetParam();
    const std::string s = Mem02ErrorCodeString(code);
    EXPECT_FALSE(s.empty());
    EXPECT_EQ(s.rfind("CX_ERR_MEM02_", 0), 0u) << s;
    EXPECT_TRUE(std::regex_match(s, std::regex("^CX_ERR_MEM02_[A-Z0-9_]+$"))) << s;
    EXPECT_STREQ(GetMem02ErrorInfo(code).cx_code, s.c_str());
}

TEST_P(Mem02ErrorMatrix, CategoryAndRetryInvariant) {
    const Mem02ErrorCode code = GetParam();
    const Mem02ErrorInfo& info = GetMem02ErrorInfo(code);
    EXPECT_TRUE(IsValidCategory(info.category));
    if (!info.retryable) {
        EXPECT_FALSE(info.retry_after_ms.has_value()) << info.cx_code;
    }
    if (info.retry_after_ms.has_value()) {
        EXPECT_GT(*info.retry_after_ms, 0) << info.cx_code;
    }
    EXPECT_GT(Mem02ErrorHttpStatus(code), 0);
}

TEST_P(Mem02ErrorMatrix, RequiredStructuredDataArms) {
    const Mem02ErrorCode code = GetParam();
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

TEST_P(Mem02ErrorMatrix, MakeRoundTripAndJson) {
    const Mem02ErrorCode code = GetParam();
    const Mem02ErrorInfo& info = GetMem02ErrorInfo(code);

    nlohmann::json sd = nlohmann::json::object();
    for (const std::string& k : RequiredStructuredDataKeys(code)) sd[k] = "v";

    auto err = MakeMem02Error(code, sd, "human detail");
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

TEST_P(Mem02ErrorMatrix, EmptyMessageFallsBackToCode) {
    const Mem02ErrorCode code = GetParam();
    auto err = MakeMem02Error(code);
    EXPECT_EQ(err.message, std::string(Mem02ErrorCodeString(code)));
}

TEST_P(Mem02ErrorMatrix, StatusBridge) {
    const Mem02ErrorCode code = GetParam();
    const StatusCode mapped = Mem02ErrorToStatusCode(code);
    EXPECT_NE(mapped, StatusCode::kOk);

    Status s = Mem02Status(code, "detail-x");
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), mapped);
    EXPECT_NE(s.message().find(Mem02ErrorCodeString(code)), std::string::npos);
    EXPECT_NE(s.message().find("detail-x"), std::string::npos);
}

TEST(Mem02ErrorMatrixAggregate, CountAndUniqueCodes) {
    EXPECT_EQ(kMem02ErrorCodeCount, 5);
    EXPECT_EQ(std::size(kAll), static_cast<size_t>(kMem02ErrorCodeCount));
    std::set<std::string> seen;
    for (Mem02ErrorCode c : kAll) {
        EXPECT_TRUE(seen.insert(Mem02ErrorCodeString(c)).second);
    }
    EXPECT_EQ(seen.size(), static_cast<size_t>(kMem02ErrorCodeCount));
}

INSTANTIATE_TEST_SUITE_P(AllCodes, Mem02ErrorMatrix, ::testing::ValuesIn(kAll));

}  // namespace
}  // namespace cortrix::memory
