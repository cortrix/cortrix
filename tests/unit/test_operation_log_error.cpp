#include <gtest/gtest.h>

#include <regex>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cortrix/agent_friendly/error.h"
#include "cortrix/observability/operation_log_error.h"

// S1 coverage: the 6 operation-log error codes — CX_ERR_OPLOG_*
// identity, category mapping, retryability, required structured_data keys, and
// the MakeOplogError boundary factory.
namespace cortrix::observability {
namespace {

using agent_friendly::ErrorCategory;

// All 6 codes, in enum order. Kept explicit (not a loop over ints) so the test
// itself documents the locked set and fails to compile if an enumerator is gone.
const std::vector<OplogErrorCode>& AllCodes() {
    static const std::vector<OplogErrorCode> codes = {
        OplogErrorCode::kInvalidFilter,
        OplogErrorCode::kInvalidTimestampRange,
        OplogErrorCode::kPaginationOutOfRange,
        OplogErrorCode::kUnauthorized,
        OplogErrorCode::kCleanupRunning,
        OplogErrorCode::kInternal,
    };
    return codes;
}

// §7.2 set size is exactly 6, and the count anchor agrees.
TEST(OplogErrorTest, SixCodesTotal) {
    EXPECT_EQ(AllCodes().size(), 6u);
    EXPECT_EQ(kOplogErrorCodeCount, 6);
}

// Every code's CX_ERR_OPLOG_* string is unique, well-formed, and carries the
// OPLOG namespace prefix (P04 ErrorResponseV1 pattern ^CX_ERR_[A-Z][A-Z_]*$).
TEST(OplogErrorTest, EveryCodeHasUniqueWellFormedCxString) {
    static const std::regex kPattern("^CX_ERR_OPLOG_[A-Z][A-Z_]*$");
    std::set<std::string> seen;
    for (OplogErrorCode code : AllCodes()) {
        std::string cx = OplogErrorCodeString(code);
        EXPECT_TRUE(std::regex_match(cx, kPattern)) << "bad code string: " << cx;
        EXPECT_TRUE(seen.insert(cx).second) << "duplicate code string: " << cx;
    }
    EXPECT_EQ(seen.size(), 6u);
}

// The §7.2 "note": CX_ERR_OPLOG_QUOTA_EXCEEDED was deliberately dropped (write-path
// auto-cleanup absorbs the cap). Assert no code carries the quota category.
TEST(OplogErrorTest, NoQuotaExceededCode) {
    for (OplogErrorCode code : AllCodes()) {
        const OplogErrorInfo& info = GetOplogErrorInfo(code);
        EXPECT_STRNE(info.cx_code, "CX_ERR_OPLOG_QUOTA_EXCEEDED");
        EXPECT_NE(info.category, ErrorCategory::kQuota)
            << info.cx_code << ": F18a §7.2 has no quota-category code";
    }
}

// Every code's category is one of the 5 GEN-Agent enum values (exercises ToString
// → the 5 contract strings).
TEST(OplogErrorTest, EveryCategoryIsOneOfFive) {
    const std::set<std::string> kFive = {"auth", "quota", "transient",
                                         "permanent", "timeout"};
    for (OplogErrorCode code : AllCodes()) {
        const OplogErrorInfo& info = GetOplogErrorInfo(code);
        EXPECT_TRUE(kFive.count(agent_friendly::ToString(info.category)) == 1)
            << "code " << info.cx_code << " has out-of-range category";
    }
}

// Exact §7.2 row attributes. CLEANUP_RUNNING is the one row with a backoff
// (5000ms base + jitter); INTERNAL is transient but carries no fixed backoff.
TEST(OplogErrorTest, RowsMatchSpec) {
    auto check = [](OplogErrorCode c, const char* cx, ErrorCategory cat,
                    bool retry, std::optional<int> after) {
        const OplogErrorInfo& i = GetOplogErrorInfo(c);
        EXPECT_STREQ(i.cx_code, cx);
        EXPECT_EQ(i.category, cat);
        EXPECT_EQ(i.retryable, retry);
        EXPECT_EQ(i.retry_after_ms, after);
    };
    check(OplogErrorCode::kInvalidFilter, "CX_ERR_OPLOG_INVALID_FILTER",
          ErrorCategory::kPermanent, false, std::nullopt);
    check(OplogErrorCode::kInvalidTimestampRange, "CX_ERR_OPLOG_INVALID_TIMESTAMP_RANGE",
          ErrorCategory::kPermanent, false, std::nullopt);
    check(OplogErrorCode::kPaginationOutOfRange, "CX_ERR_OPLOG_PAGINATION_OUT_OF_RANGE",
          ErrorCategory::kPermanent, false, std::nullopt);
    check(OplogErrorCode::kUnauthorized, "CX_ERR_OPLOG_UNAUTHORIZED",
          ErrorCategory::kAuth, false, std::nullopt);
    check(OplogErrorCode::kCleanupRunning, "CX_ERR_OPLOG_CLEANUP_RUNNING",
          ErrorCategory::kTransient, true, 5000);
    check(OplogErrorCode::kInternal, "CX_ERR_OPLOG_INTERNAL",
          ErrorCategory::kTransient, true, std::nullopt);
    EXPECT_EQ(kOplogCleanupRetryBaseMs, 5000);
}

// Required structured_data keys mirror the §7.2 "structured_data required" column 1:1.
TEST(OplogErrorTest, RequiredStructuredDataKeysMatchSpec) {
    auto keys = [](OplogErrorCode c) {
        return std::set<std::string>(RequiredStructuredDataKeys(c).begin(),
                                     RequiredStructuredDataKeys(c).end());
    };
    EXPECT_EQ(keys(OplogErrorCode::kInvalidFilter),
              (std::set<std::string>{"invalid_field", "reason"}));
    EXPECT_EQ(keys(OplogErrorCode::kInvalidTimestampRange),
              (std::set<std::string>{"from", "to"}));
    EXPECT_EQ(keys(OplogErrorCode::kPaginationOutOfRange),
              (std::set<std::string>{"offset", "total_count"}));
    EXPECT_EQ(keys(OplogErrorCode::kUnauthorized),
              (std::set<std::string>{"required_role"}));
    EXPECT_EQ(keys(OplogErrorCode::kCleanupRunning),
              (std::set<std::string>{"retry_at_ms"}));
    EXPECT_EQ(keys(OplogErrorCode::kInternal),
              (std::set<std::string>{"error_id"}));
}

// HasRequiredStructuredData: complete body passes, missing-key body fails.
TEST(OplogErrorTest, HasRequiredStructuredDataChecksEveryKey) {
    EXPECT_TRUE(HasRequiredStructuredData(
        OplogErrorCode::kInvalidFilter,
        {{"invalid_field", "limit"}, {"reason", "exceeds 200"}}));
    // missing "reason"
    EXPECT_FALSE(HasRequiredStructuredData(
        OplogErrorCode::kInvalidFilter, {{"invalid_field", "limit"}}));
    // non-object payload for a code that requires keys → incomplete
    EXPECT_FALSE(HasRequiredStructuredData(
        OplogErrorCode::kUnauthorized, nlohmann::json("not-an-object")));
}

// MakeOplogError fills the boundary error from the registry and attaches
// structured_data; ToJson then yields the AGENT_FRIENDLY §3.1 body shape.
TEST(OplogErrorTest, MakeOplogErrorBuildsAgentFriendlyBody) {
    nlohmann::json sd = {{"required_role", "admin"}};
    auto err = MakeOplogError(OplogErrorCode::kUnauthorized, sd);
    EXPECT_EQ(err.code, "CX_ERR_OPLOG_UNAUTHORIZED");
    EXPECT_FALSE(err.retryable);
    EXPECT_EQ(err.category, ErrorCategory::kAuth);
    EXPECT_FALSE(err.retry_after_ms.has_value());
    ASSERT_TRUE(err.structured_data.has_value());
    EXPECT_EQ((*err.structured_data)["required_role"], "admin");
    EXPECT_EQ(err.message, "CX_ERR_OPLOG_UNAUTHORIZED");  // default → code string

    nlohmann::json body = agent_friendly::ToJson(err);
    EXPECT_EQ(body["code"], "CX_ERR_OPLOG_UNAUTHORIZED");
    EXPECT_EQ(body["category"], "auth");
    EXPECT_TRUE(body["retry_after_ms"].is_null());
    EXPECT_EQ(body["structured_data"]["required_role"], "admin");
}

// The transient CLEANUP_RUNNING code surfaces its 5000ms backoff through JSON.
TEST(OplogErrorTest, CleanupRunningExposesRetryAfterInJson) {
    auto err = MakeOplogError(OplogErrorCode::kCleanupRunning,
                              {{"retry_at_ms", 1747584005321LL}}, "cleanup in progress");
    EXPECT_TRUE(err.retryable);
    nlohmann::json body = agent_friendly::ToJson(err);
    EXPECT_EQ(body["retryable"], true);
    EXPECT_EQ(body["retry_after_ms"], 5000);
    EXPECT_EQ(body["category"], "transient");
    EXPECT_EQ(body["message"], "cleanup in progress");
}

// OplogStatus bridges to a plain Status, prefixing the message with the CX token
// so the exact identity is recoverable at the boundary (F-FREEZE-1 surface).
TEST(OplogErrorTest, OplogStatusCarriesCxTokenAndMapsCode) {
    Status s = OplogStatus(OplogErrorCode::kUnauthorized, "user 'bob' not admin");
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kPermissionDenied);
    EXPECT_NE(s.message().find("CX_ERR_OPLOG_UNAUTHORIZED"), std::string::npos);
    EXPECT_NE(s.message().find("user 'bob' not admin"), std::string::npos);

    // coarse mapping spot-checks
    EXPECT_EQ(OplogErrorToStatusCode(OplogErrorCode::kInvalidFilter),
              StatusCode::kInvalidArgument);
    EXPECT_EQ(OplogErrorToStatusCode(OplogErrorCode::kCleanupRunning),
              StatusCode::kUnavailable);
    EXPECT_EQ(OplogErrorToStatusCode(OplogErrorCode::kInternal),
              StatusCode::kInternal);
}

}  // namespace
}  // namespace cortrix::observability
