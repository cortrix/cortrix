#include <gtest/gtest.h>

#include <optional>
#include <regex>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cortrix/agent_friendly/error.h"
#include "cortrix/catalog/catalog_error.h"
#include "cortrix/common/status.h"

// Exhaustive error-registry matrix for F12 catalog (src/catalog/catalog_error.*).
// One TEST_P case per CatalogErrorCode (ValuesIn over the full enum), so appending a
// code auto-extends coverage. Unique suite name (CatalogErrorMatrix) — no clash with
// the existing CatalogErrorTest in test_catalog_error.cpp.
namespace cortrix::catalog {
namespace {

using agent_friendly::ErrorCategory;

// All codes, in enum order. Explicit (not a loop over ints) so the test documents
// the locked set and fails to compile if an enumerator is renamed/removed.
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

const std::set<std::string>& ValidCategoryStrings() {
    static const std::set<std::string> kFive = {"auth", "quota", "transient",
                                                "permanent", "timeout"};
    return kFive;
}

const std::set<StatusCode>& ValidStatusCodes() {
    static const std::set<StatusCode> kCodes = {
        StatusCode::kInvalidArgument, StatusCode::kNotFound,
        StatusCode::kAlreadyExists,   StatusCode::kPermissionDenied,
        StatusCode::kUnauthenticated, StatusCode::kInternal,
        StatusCode::kUnavailable};
    return kCodes;
}

class CatalogErrorMatrix : public ::testing::TestWithParam<CatalogErrorCode> {};

// Code string is well-formed (CX_ERR_*/CX_WARN_* token). Catalog has only CX_ERR_.
TEST_P(CatalogErrorMatrix, CodeStringWellFormed) {
    static const std::regex kPattern("^CX_(ERR|WARN)_[A-Z][A-Z0-9_]*$");
    const std::string cx = CatalogErrorCodeString(GetParam());
    EXPECT_TRUE(std::regex_match(cx, kPattern)) << "bad code string: " << cx;
    // GetXxxErrorInfo().cx_code agrees with the string accessor.
    const CatalogErrorInfo& info = GetCatalogErrorInfo(GetParam());
    EXPECT_EQ(cx, std::string(info.cx_code));
}

// Category is one of the 5 GEN-Agent enum values (exercises ToString too).
TEST_P(CatalogErrorMatrix, CategoryInValidSet) {
    const CatalogErrorInfo& info = GetCatalogErrorInfo(GetParam());
    EXPECT_EQ(ValidCategoryStrings().count(agent_friendly::ToString(info.category)), 1u)
        << "code " << info.cx_code << " has out-of-range category";
}

// retry_after_ms present implies retryable (universal invariant across modules).
// retryable backoffs are strictly positive when present.
TEST_P(CatalogErrorMatrix, RetryAfterImpliesRetryable) {
    const CatalogErrorInfo& info = GetCatalogErrorInfo(GetParam());
    if (info.retry_after_ms.has_value()) {
        EXPECT_TRUE(info.retryable) << info.cx_code;
        EXPECT_GT(*info.retry_after_ms, 0) << info.cx_code;
    }
    // In catalog every retryable code also carries a backoff (module-uniform here).
    EXPECT_EQ(info.retryable, info.retry_after_ms.has_value()) << info.cx_code;
}

// RequiredStructuredDataKeys + HasRequiredStructuredData: all-present true, any
// missing key false, and the non-object arm (true iff no keys required).
TEST_P(CatalogErrorMatrix, RequiredStructuredDataContract) {
    const CatalogErrorCode code = GetParam();
    const std::vector<std::string>& keys = RequiredStructuredDataKeys(code);

    // Build an object containing every required key.
    nlohmann::json full = nlohmann::json::object();
    for (const std::string& k : keys) full[k] = "v";
    EXPECT_TRUE(HasRequiredStructuredData(code, full));

    // Drop one key -> must be false (only when there is at least one key).
    if (!keys.empty()) {
        nlohmann::json missing = full;
        missing.erase(keys.front());
        EXPECT_FALSE(HasRequiredStructuredData(code, missing))
            << "missing key " << keys.front();
    }

    // Non-object structured_data satisfies only an empty key list.
    EXPECT_EQ(HasRequiredStructuredData(code, nlohmann::json("x")), keys.empty());
    EXPECT_EQ(HasRequiredStructuredData(code, nlohmann::json(nullptr)), keys.empty());
}

// Make...Error round-trips the registry attributes + structured_data + JSON body.
TEST_P(CatalogErrorMatrix, MakeErrorRoundTrips) {
    const CatalogErrorCode code = GetParam();
    const CatalogErrorInfo& info = GetCatalogErrorInfo(code);
    nlohmann::json sd = {{"probe", 1}};

    auto err = MakeCatalogError(code, sd, "human detail");
    EXPECT_EQ(err.code, std::string(info.cx_code));
    EXPECT_EQ(err.category, info.category);
    EXPECT_EQ(err.retryable, info.retryable);
    EXPECT_EQ(err.retry_after_ms, info.retry_after_ms);
    EXPECT_EQ(err.message, "human detail");
    ASSERT_TRUE(err.structured_data.has_value());
    EXPECT_EQ((*err.structured_data)["probe"], 1);

    nlohmann::json body = agent_friendly::ToJson(err);
    EXPECT_EQ(body["code"], std::string(info.cx_code));
    EXPECT_EQ(body["category"], agent_friendly::ToString(info.category));
    EXPECT_EQ(body["retryable"], info.retryable);
    if (info.retry_after_ms.has_value()) {
        EXPECT_EQ(body["retry_after_ms"], *info.retry_after_ms);
    } else {
        EXPECT_TRUE(body["retry_after_ms"].is_null());
    }
    EXPECT_EQ(body["structured_data"]["probe"], 1);
}

// Empty message falls back to the CX token (never an empty string).
TEST_P(CatalogErrorMatrix, EmptyMessageFallsBackToCode) {
    const CatalogErrorCode code = GetParam();
    auto err = MakeCatalogError(code);
    EXPECT_EQ(err.message, std::string(CatalogErrorCodeString(code)));
}

// ToStatusCode maps into the valid StatusCode bucket set, and the Status bridge
// agrees with it (non-OK, token recoverable from the message).
TEST_P(CatalogErrorMatrix, StatusCodeBucketAndBridge) {
    const CatalogErrorCode code = GetParam();
    const StatusCode sc = CatalogErrorToStatusCode(code);
    EXPECT_EQ(ValidStatusCodes().count(sc), 1u);
    EXPECT_NE(sc, StatusCode::kOk);

    Status s = CatalogStatus(code, "detail-z");
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), sc);
    EXPECT_NE(s.message().find(CatalogErrorCodeString(code)), std::string::npos);
    EXPECT_NE(s.message().find("detail-z"), std::string::npos);

    // Empty detail -> message is exactly the CX token.
    Status bare = CatalogStatus(code);
    EXPECT_EQ(bare.message(), std::string(CatalogErrorCodeString(code)));
}

INSTANTIATE_TEST_SUITE_P(AllCatalogCodes, CatalogErrorMatrix,
                         ::testing::ValuesIn(AllCodes()));

// Non-parameterized guard: every code's CX string is unique, and the count anchor
// matches the enumerated set.
TEST(CatalogErrorMatrixGuard, CodeStringsAreUniqueAndCountMatches) {
    std::set<std::string> seen;
    for (CatalogErrorCode code : AllCodes()) {
        EXPECT_TRUE(seen.insert(CatalogErrorCodeString(code)).second)
            << "duplicate: " << CatalogErrorCodeString(code);
    }
    EXPECT_EQ(seen.size(), AllCodes().size());
    EXPECT_EQ(static_cast<int>(AllCodes().size()), kCatalogErrorCodeCount);
}

}  // namespace
}  // namespace cortrix::catalog
