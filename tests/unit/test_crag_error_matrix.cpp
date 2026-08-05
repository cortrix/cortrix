#include <gtest/gtest.h>

#include <regex>
#include <set>
#include <string>
#include <vector>

#include "cortrix/agent_friendly/error.h"
#include "cortrix/retrieval/crag_error.h"

// Exhaustive parameterized error-registry sweep for CRAG (§4.3, 4 codes).
// Distinct suite name (CragErrorMatrix) from test_crag_error.cpp.
namespace cortrix::retrieval {
namespace {

using agent_friendly::ErrorCategory;

const CragErrorCode kAll[] = {
    CragErrorCode::kClassifierLoadFailed,
    CragErrorCode::kInferenceFailed,
    CragErrorCode::kThresholdInvalid,
    CragErrorCode::kFallbackTriggered,
};

bool IsValidCategory(ErrorCategory c) {
    return c == ErrorCategory::kAuth || c == ErrorCategory::kQuota ||
           c == ErrorCategory::kTransient || c == ErrorCategory::kPermanent ||
           c == ErrorCategory::kTimeout;
}

class CragErrorMatrix : public ::testing::TestWithParam<CragErrorCode> {};

TEST_P(CragErrorMatrix, CodeStringFormat) {
    const CragErrorCode code = GetParam();
    const std::string s = CragErrorCodeString(code);
    EXPECT_FALSE(s.empty());
    EXPECT_EQ(s.rfind("CX_ERR_CRAG_", 0), 0u) << s;
    EXPECT_TRUE(std::regex_match(s, std::regex("^CX_ERR_CRAG_[A-Z0-9_]+$"))) << s;
    EXPECT_STREQ(GetCragErrorInfo(code).cx_code, s.c_str());
}

TEST_P(CragErrorMatrix, CategoryAndRetryInvariant) {
    const CragErrorCode code = GetParam();
    const CragErrorInfo& info = GetCragErrorInfo(code);
    EXPECT_TRUE(IsValidCategory(info.category));
    // Non-retryable codes must carry no retry hint. (kFallbackTriggered is
    // retryable with a null interval by design, so the converse is NOT asserted.)
    if (!info.retryable) {
        EXPECT_FALSE(info.retry_after_ms.has_value()) << info.cx_code;
    }
    if (info.retry_after_ms.has_value()) {
        EXPECT_GT(*info.retry_after_ms, 0) << info.cx_code;
    }
}

TEST_P(CragErrorMatrix, RequiredStructuredDataArms) {
    const CragErrorCode code = GetParam();
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

TEST_P(CragErrorMatrix, MakeRoundTripAndJson) {
    const CragErrorCode code = GetParam();
    const CragErrorInfo& info = GetCragErrorInfo(code);

    nlohmann::json sd = nlohmann::json::object();
    for (const std::string& k : RequiredStructuredDataKeys(code)) sd[k] = "v";

    auto err = MakeCragError(code, sd, "human detail");
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

TEST_P(CragErrorMatrix, EmptyMessageFallsBackToCode) {
    const CragErrorCode code = GetParam();
    auto err = MakeCragError(code);
    EXPECT_EQ(err.message, std::string(CragErrorCodeString(code)));
}

TEST_P(CragErrorMatrix, StatusBridge) {
    const CragErrorCode code = GetParam();
    const StatusCode mapped = CragErrorToStatusCode(code);
    EXPECT_NE(mapped, StatusCode::kOk);

    Status s = CragStatus(code, "detail-x");
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), mapped);
    EXPECT_NE(s.message().find(CragErrorCodeString(code)), std::string::npos);
    EXPECT_NE(s.message().find("detail-x"), std::string::npos);
}

TEST(CragErrorMatrixAggregate, CountAndUniqueCodes) {
    EXPECT_EQ(kCragErrorCodeCount, 4);
    EXPECT_EQ(std::size(kAll), static_cast<size_t>(kCragErrorCodeCount));
    std::set<std::string> seen;
    for (CragErrorCode c : kAll) {
        EXPECT_TRUE(seen.insert(CragErrorCodeString(c)).second);
    }
    EXPECT_EQ(seen.size(), static_cast<size_t>(kCragErrorCodeCount));
}

INSTANTIATE_TEST_SUITE_P(AllCodes, CragErrorMatrix, ::testing::ValuesIn(kAll));

}  // namespace
}  // namespace cortrix::retrieval
