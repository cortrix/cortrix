#include <gtest/gtest.h>

#include <regex>
#include <set>
#include <string>

#include "cortrix/agent_friendly/error.h"
#include "cortrix/spc/cleaning_errors.h"

// Exhaustive parameterized error-registry sweep for F10 data-cleaning (§5.1, 7
// codes). Distinct suite name (CleaningErrorMatrix) from test_cleaning_errors.cpp.
//
// NOTE: cleaning_errors.h does NOT expose RequiredStructuredDataKeys /
// HasRequiredStructuredData (no per-code structured-data contract); instead
// MakeCleaningError injects the CX_ERR_* identity into structured_data["code"].
// The structured-data arms therefore cover that injection rather than a key list.
namespace cortrix::spc {
namespace {

using agent_friendly::ErrorCategory;

const CleaningErrorCode kAll[] = {
    CleaningErrorCode::kDedupVectorInvalid,
    CleaningErrorCode::kDedupThresholdRange,
    CleaningErrorCode::kAnomalyConfigInvalid,
    CleaningErrorCode::kPluginTimeout,
    CleaningErrorCode::kPluginException,
    CleaningErrorCode::kNsConfigMergeFailed,
    CleaningErrorCode::kInternalError,
};

bool IsValidCategory(ErrorCategory c) {
    return c == ErrorCategory::kAuth || c == ErrorCategory::kQuota ||
           c == ErrorCategory::kTransient || c == ErrorCategory::kPermanent ||
           c == ErrorCategory::kTimeout;
}

class CleaningErrorMatrix : public ::testing::TestWithParam<CleaningErrorCode> {};

TEST_P(CleaningErrorMatrix, CodeStringFormat) {
    const CleaningErrorCode code = GetParam();
    const std::string s = CleaningErrorCodeString(code);
    EXPECT_FALSE(s.empty());
    EXPECT_EQ(s.rfind("CX_ERR_F10_", 0), 0u) << s;
    EXPECT_TRUE(std::regex_match(s, std::regex("^CX_ERR_F10_[A-Z0-9_]+$"))) << s;
    EXPECT_STREQ(GetCleaningErrorInfo(code).cx_code, s.c_str());
}

TEST_P(CleaningErrorMatrix, CategoryAndRetryInvariant) {
    const CleaningErrorCode code = GetParam();
    const CleaningErrorInfo& info = GetCleaningErrorInfo(code);
    EXPECT_TRUE(IsValidCategory(info.category));
    if (!info.retryable) {
        EXPECT_FALSE(info.retry_after_ms.has_value()) << info.cx_code;
    }
    if (info.retry_after_ms.has_value()) {
        EXPECT_GT(*info.retry_after_ms, 0) << info.cx_code;
    }
}

TEST_P(CleaningErrorMatrix, MakeInjectsCodeIntoStructuredData) {
    const CleaningErrorCode code = GetParam();
    const CleaningErrorInfo& info = GetCleaningErrorInfo(code);

    // Empty object: Make injects structured_data["code"] = cx_code.
    auto err = MakeCleaningError(code);
    ASSERT_TRUE(err.structured_data.has_value());
    ASSERT_TRUE(err.structured_data->is_object());
    EXPECT_EQ((*err.structured_data)["code"], std::string(info.cx_code));

    // Caller-supplied "code" is preserved (not overwritten).
    nlohmann::json supplied = {{"code", "CALLER_OWNED"}, {"x", 1}};
    auto err2 = MakeCleaningError(code, supplied, "human detail");
    ASSERT_TRUE(err2.structured_data.has_value());
    EXPECT_EQ((*err2.structured_data)["code"], "CALLER_OWNED");
    EXPECT_EQ((*err2.structured_data)["x"], 1);
}

TEST_P(CleaningErrorMatrix, MakeRoundTripAndJson) {
    const CleaningErrorCode code = GetParam();
    const CleaningErrorInfo& info = GetCleaningErrorInfo(code);

    auto err = MakeCleaningError(code, nlohmann::json::object(), "human detail");
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

TEST_P(CleaningErrorMatrix, EmptyMessageFallsBackToCode) {
    const CleaningErrorCode code = GetParam();
    auto err = MakeCleaningError(code);
    EXPECT_EQ(err.message, std::string(CleaningErrorCodeString(code)));
}

TEST_P(CleaningErrorMatrix, StatusBridge) {
    const CleaningErrorCode code = GetParam();
    const StatusCode mapped = CleaningErrorToStatusCode(code);
    EXPECT_NE(mapped, StatusCode::kOk);

    Status s = CleaningStatus(code, "detail-x");
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), mapped);
    EXPECT_NE(s.message().find(CleaningErrorCodeString(code)), std::string::npos);
    EXPECT_NE(s.message().find("detail-x"), std::string::npos);
}

TEST(CleaningErrorMatrixAggregate, CountAndUniqueCodes) {
    EXPECT_EQ(kCleaningErrorCodeCount, 7);
    EXPECT_EQ(std::size(kAll), static_cast<size_t>(kCleaningErrorCodeCount));
    std::set<std::string> seen;
    for (CleaningErrorCode c : kAll) {
        EXPECT_TRUE(seen.insert(CleaningErrorCodeString(c)).second);
    }
    EXPECT_EQ(seen.size(), static_cast<size_t>(kCleaningErrorCodeCount));
}

INSTANTIATE_TEST_SUITE_P(AllCodes, CleaningErrorMatrix, ::testing::ValuesIn(kAll));

}  // namespace
}  // namespace cortrix::spc
