#include <gtest/gtest.h>

#include <regex>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cortrix/agent_friendly/error.h"
#include "cortrix/catalog/catalog_error.h"

// S2.1 coverage: the 24 catalog error codes — CX_ERR_ identity,
// category mapping, retryability, and the MakeCatalogError boundary factory.
namespace cortrix::catalog {
namespace {

using agent_friendly::ErrorCategory;

// All 24 codes, in enum order. Kept explicit (not a loop over ints) so the test
// itself documents the locked set and fails to compile if an enumerator is gone.
const std::vector<CatalogErrorCode>& AllCodes() {
    static const std::vector<CatalogErrorCode> codes = {
        CatalogErrorCode::kNsNotFound,
        CatalogErrorCode::kNsUnauthorized,
        CatalogErrorCode::kNsIsolated,
        CatalogErrorCode::kNsVisibilityDenied,
        CatalogErrorCode::kNsAlreadyExists,
        CatalogErrorCode::kUnitNotFound,
        CatalogErrorCode::kUnitNotSealed,
        CatalogErrorCode::kUnitArchived,
        CatalogErrorCode::kUnitOwnerRemote,
        CatalogErrorCode::kTenantNotFound,
        CatalogErrorCode::kTenantQuotaExceeded,
        CatalogErrorCode::kSchemaVersionMismatch,
        CatalogErrorCode::kSchemaMigrationFailed,
        CatalogErrorCode::kInvalidConfigJson,
        CatalogErrorCode::kCatalogLocked,
        CatalogErrorCode::kBloomFilterNotReady,
        CatalogErrorCode::kTransactionRetry,
        CatalogErrorCode::kContentHashMismatch,
        CatalogErrorCode::kRefCountNegative,
        CatalogErrorCode::kInternalError,
        CatalogErrorCode::kNotImplemented,
        CatalogErrorCode::kNsQuotaExceeded,
        CatalogErrorCode::kNsResourceBudgetExceeded,
        CatalogErrorCode::kNsPoolInternal,
    };
    return codes;
}

// set size is exactly 24, and the count anchor agrees.
TEST(CatalogErrorTest, TwentyFourCodesTotal) {
    EXPECT_EQ(AllCodes().size(), 24u);
    EXPECT_EQ(kCatalogErrorCodeCount, 24);
}

// Every code's CX_ERR_* string is unique and matches API spec ErrorResponseV1 pattern
// ^CX_ERR_[A-Z][A-Z_]*$ (catalog fixed the missing-prefix codes).
TEST(CatalogErrorTest, EveryCodeHasUniqueWellFormedCxString) {
    static const std::regex kPattern("^CX_ERR_[A-Z][A-Z_]*$");
    std::set<std::string> seen;
    for (CatalogErrorCode code : AllCodes()) {
        std::string cx = CatalogErrorCodeString(code);
        EXPECT_TRUE(std::regex_match(cx, kPattern)) << "bad code string: " << cx;
        EXPECT_TRUE(seen.insert(cx).second) << "duplicate code string: " << cx;
    }
    EXPECT_EQ(seen.size(), 24u);
}

// Every code's category is one of the 5 GEN-Agent enum values (trivially true via
// the enum, but this also exercises ToString → the 5 contract strings).
TEST(CatalogErrorTest, EveryCategoryIsOneOfFive) {
    const std::set<std::string> kFive = {"auth", "quota", "transient",
                                         "permanent", "timeout"};
    for (CatalogErrorCode code : AllCodes()) {
        const CatalogErrorInfo& info = GetCatalogErrorInfo(code);
        EXPECT_TRUE(kFive.count(agent_friendly::ToString(info.category)) == 1)
            << "code " << info.cx_code << " has out-of-range category";
    }
}

// retryable ⇔ retry_after_ms present (transient codes carry a backoff; permanent
// auth / quota codes do not). Mirrors the retry_after_ms column.
TEST(CatalogErrorTest, RetryableIffRetryAfterPresent) {
    for (CatalogErrorCode code : AllCodes()) {
        const CatalogErrorInfo& info = GetCatalogErrorInfo(code);
        EXPECT_EQ(info.retryable, info.retry_after_ms.has_value())
            << "code " << info.cx_code << ": retryable/retry_after_ms mismatch";
        if (info.retryable) {
            EXPECT_GT(*info.retry_after_ms, 0) << info.cx_code;
            EXPECT_EQ(info.category, ErrorCategory::kTransient)
                << info.cx_code << ": only transient codes are retryable in";
        }
    }
}

// Spot-check the exact rows that downstream consumers depend on.
TEST(CatalogErrorTest, SpecificRowsMatchSpec) {
    auto check = [](CatalogErrorCode c, const char* cx, ErrorCategory cat,
                    bool retry, std::optional<int> after) {
        const CatalogErrorInfo& i = GetCatalogErrorInfo(c);
        EXPECT_STREQ(i.cx_code, cx);
        EXPECT_EQ(i.category, cat);
        EXPECT_EQ(i.retryable, retry);
        EXPECT_EQ(i.retry_after_ms, after);
    };
    check(CatalogErrorCode::kNsNotFound, "CX_ERR_NS_NOT_FOUND",
          ErrorCategory::kPermanent, false, std::nullopt);
    check(CatalogErrorCode::kNsUnauthorized, "CX_ERR_NS_UNAUTHORIZED",
          ErrorCategory::kAuth, false, std::nullopt);
    check(CatalogErrorCode::kUnitNotSealed, "CX_ERR_UNIT_NOT_SEALED",
          ErrorCategory::kTransient, true, 5000);
    check(CatalogErrorCode::kCatalogLocked, "CX_ERR_CATALOG_LOCKED",
          ErrorCategory::kTransient, true, 100);
    check(CatalogErrorCode::kTransactionRetry, "CX_ERR_TRANSACTION_RETRY",
          ErrorCategory::kTransient, true, 50);
    check(CatalogErrorCode::kBloomFilterNotReady, "CX_ERR_BLOOM_FILTER_NOT_READY",
          ErrorCategory::kTransient, true, 1000);
    check(CatalogErrorCode::kNsQuotaExceeded, "CX_ERR_NS_QUOTA_EXCEEDED",
          ErrorCategory::kQuota, false, std::nullopt);
    // Phase-2-reserved code is present with its attributes.
    check(CatalogErrorCode::kUnitArchived, "CX_ERR_UNIT_ARCHIVED",
          ErrorCategory::kTransient, true, 10000);
}

// MakeCatalogError fills the boundary error from the registry and attaches
// structured_data; ToJson then yields the Agent-friendly contract body shape.
TEST(CatalogErrorTest, MakeCatalogErrorBuildsAgentFriendlyBody) {
    nlohmann::json sd = {{"ns_id", "ns-42"}};
    auto err = MakeCatalogError(CatalogErrorCode::kNsNotFound, sd);
    EXPECT_EQ(err.code, "CX_ERR_NS_NOT_FOUND");
    EXPECT_FALSE(err.retryable);
    EXPECT_EQ(err.category, ErrorCategory::kPermanent);
    EXPECT_FALSE(err.retry_after_ms.has_value());
    ASSERT_TRUE(err.structured_data.has_value());
    EXPECT_EQ((*err.structured_data)["ns_id"], "ns-42");
    // default message falls back to the code string.
    EXPECT_EQ(err.message, "CX_ERR_NS_NOT_FOUND");

    nlohmann::json body = agent_friendly::ToJson(err);
    EXPECT_EQ(body["code"], "CX_ERR_NS_NOT_FOUND");
    EXPECT_EQ(body["category"], "permanent");
    EXPECT_TRUE(body["retry_after_ms"].is_null());
    EXPECT_EQ(body["structured_data"]["ns_id"], "ns-42");
}

// A transient code surfaces retry_after_ms through the JSON body for the Agent.
TEST(CatalogErrorTest, RetryableCodeExposesRetryAfterInJson) {
    auto err = MakeCatalogError(CatalogErrorCode::kCatalogLocked,
                                {{"lock_holder", "writer-1"}, {"wait_timeout_ms", 250}},
                                "catalog busy");
    EXPECT_TRUE(err.retryable);
    nlohmann::json body = agent_friendly::ToJson(err);
    EXPECT_EQ(body["retryable"], true);
    EXPECT_EQ(body["retry_after_ms"], 100);
    EXPECT_EQ(body["category"], "transient");
    EXPECT_EQ(body["message"], "catalog busy");
}

}  // namespace
}  // namespace cortrix::catalog
