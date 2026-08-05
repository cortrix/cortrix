#include <gtest/gtest.h>

#include <regex>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cortrix/agent_friendly/error.h"
#include "cortrix/spc/cleaning_errors.h"

// Cleaning (D9): the 7 CX_ERR_CLEANING_* identities + category/retryability registry
// + MakeCleaningError boundary factory. Mirrors the catalog/parser error tests.
namespace cortrix::spc {
namespace {

using agent_friendly::ErrorCategory;

const std::vector<CleaningErrorCode>& AllCodes() {
    static const std::vector<CleaningErrorCode> codes = {
        CleaningErrorCode::kDedupVectorInvalid,
        CleaningErrorCode::kDedupThresholdRange,
        CleaningErrorCode::kAnomalyConfigInvalid,
        CleaningErrorCode::kPluginTimeout,
        CleaningErrorCode::kPluginException,
        CleaningErrorCode::kNsConfigMergeFailed,
        CleaningErrorCode::kInternalError,
    };
    return codes;
}

TEST(CleaningErrorTest, SevenCodesTotal) {
    EXPECT_EQ(AllCodes().size(), 7u);
    EXPECT_EQ(kCleaningErrorCodeCount, 7);
}

TEST(CleaningErrorTest, EveryCodeHasUniqueWellFormedCxString) {
    // All cleaning codes carry the CX_ERR_CLEANING_ prefix (the family marker).
    static const std::regex kPattern("^CX_ERR_CLEANING_[A-Z][A-Z_]*$");
    std::set<std::string> seen;
    for (CleaningErrorCode code : AllCodes()) {
        std::string cx = CleaningErrorCodeString(code);
        EXPECT_TRUE(std::regex_match(cx, kPattern)) << "bad code string: " << cx;
        EXPECT_TRUE(seen.insert(cx).second) << "duplicate: " << cx;
    }
    EXPECT_EQ(seen.size(), 7u);
}

TEST(CleaningErrorTest, RetryableIffRetryAfterPresent) {
    for (CleaningErrorCode code : AllCodes()) {
        const CleaningErrorInfo& info = GetCleaningErrorInfo(code);
        EXPECT_EQ(info.retryable, info.retry_after_ms.has_value()) << info.cx_code;
        if (info.retryable) EXPECT_GT(*info.retry_after_ms, 0) << info.cx_code;
    }
}

// Exact §5.1 rows.
TEST(CleaningErrorTest, RowsMatchSpec) {
    auto check = [](CleaningErrorCode c, const char* cx, ErrorCategory cat,
                    bool retry, std::optional<int> after) {
        const CleaningErrorInfo& i = GetCleaningErrorInfo(c);
        EXPECT_STREQ(i.cx_code, cx);
        EXPECT_EQ(i.category, cat);
        EXPECT_EQ(i.retryable, retry);
        EXPECT_EQ(i.retry_after_ms, after);
    };
    check(CleaningErrorCode::kDedupVectorInvalid, "CX_ERR_CLEANING_DEDUP_VECTOR_INVALID",
          ErrorCategory::kPermanent, false, std::nullopt);
    check(CleaningErrorCode::kDedupThresholdRange, "CX_ERR_CLEANING_DEDUP_THRESHOLD_RANGE",
          ErrorCategory::kPermanent, false, std::nullopt);
    check(CleaningErrorCode::kAnomalyConfigInvalid, "CX_ERR_CLEANING_ANOMALY_CONFIG_INVALID",
          ErrorCategory::kPermanent, false, std::nullopt);
    check(CleaningErrorCode::kPluginTimeout, "CX_ERR_CLEANING_PLUGIN_TIMEOUT",
          ErrorCategory::kTimeout, true, 1000);
    check(CleaningErrorCode::kPluginException, "CX_ERR_CLEANING_PLUGIN_EXCEPTION",
          ErrorCategory::kPermanent, false, std::nullopt);
    check(CleaningErrorCode::kNsConfigMergeFailed, "CX_ERR_CLEANING_NS_CONFIG_MERGE_FAILED",
          ErrorCategory::kPermanent, false, std::nullopt);
    check(CleaningErrorCode::kInternalError, "CX_ERR_CLEANING_INTERNAL_ERROR",
          ErrorCategory::kTransient, true, 2000);
}

// MakeCleaningError fills the boundary body from the registry; ToJson yields the
// AGENT_FRIENDLY §3.1 shape.
TEST(CleaningErrorTest, MakeCleaningErrorBuildsAgentFriendlyBody) {
    auto err = MakeCleaningError(CleaningErrorCode::kPluginTimeout,
                                 {{"plugin", "redactor"}, {"timeout_ms", 3000}},
                                 "plugin too slow");
    EXPECT_EQ(err.code, "CX_ERR_CLEANING_PLUGIN_TIMEOUT");
    EXPECT_TRUE(err.retryable);
    EXPECT_EQ(err.category, ErrorCategory::kTimeout);
    ASSERT_TRUE(err.retry_after_ms.has_value());
    EXPECT_EQ(*err.retry_after_ms, 1000);
    EXPECT_EQ((*err.structured_data)["code"], "CX_ERR_CLEANING_PLUGIN_TIMEOUT");
    EXPECT_EQ((*err.structured_data)["plugin"], "redactor");

    nlohmann::json body = agent_friendly::ToJson(err);
    EXPECT_EQ(body["code"], "CX_ERR_CLEANING_PLUGIN_TIMEOUT");
    EXPECT_EQ(body["category"], "timeout");
    EXPECT_EQ(body["retry_after_ms"], 1000);
    EXPECT_EQ(body["message"], "plugin too slow");
}

TEST(CleaningErrorTest, NonRetryableHasNullRetryAfterInJson) {
    auto err = MakeCleaningError(CleaningErrorCode::kDedupThresholdRange,
                                 {{"threshold", 1.5}});
    nlohmann::json body = agent_friendly::ToJson(err);
    EXPECT_EQ(body["retryable"], false);
    EXPECT_TRUE(body["retry_after_ms"].is_null());
}

TEST(CleaningErrorTest, CleaningStatusCarriesCxToken) {
    Status s = CleaningStatus(CleaningErrorCode::kAnomalyConfigInvalid, "max_chunk_chars=0");
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kInvalidArgument);
    EXPECT_NE(s.message().find("CX_ERR_CLEANING_ANOMALY_CONFIG_INVALID"), std::string::npos);
    EXPECT_NE(s.message().find("max_chunk_chars=0"), std::string::npos);
    EXPECT_EQ(CleaningErrorToStatusCode(CleaningErrorCode::kInternalError),
              StatusCode::kUnavailable);
}

}  // namespace
}  // namespace cortrix::spc
