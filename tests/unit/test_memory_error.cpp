#include <gtest/gtest.h>

#include <set>
#include <string>

#include "cortrix/memory/memory_error.h"

// S5 coverage: the memory transparency error model (template A) — all 5 CX_ERR_MEMORY_* identities,
// their §4.3.4.bis attributes (http/category/retryable/retry_after_ms/structured_data
// keys), the AgentFriendlyError builder, and the Status bridge.
namespace cortrix::memory::transparency {
namespace {

using agent_friendly::ErrorCategory;

// Every enum value the suite walks (kept in sync with kMemoryErrorCodeCount).
constexpr MemoryErrorCode kAll[] = {
    MemoryErrorCode::kMemoryNotFound,
    MemoryErrorCode::kUserMismatch,
    MemoryErrorCode::kAlreadyInvalidated,
    MemoryErrorCode::kInvalidateFailed,
    MemoryErrorCode::kQuota,
};

TEST(MemoryErrorTest, CountMatchesEnumeration) {
    EXPECT_EQ(kMemoryErrorCodeCount, 5);
    EXPECT_EQ(std::size(kAll), static_cast<size_t>(kMemoryErrorCodeCount));
}

TEST(MemoryErrorTest, AllCodesHaveUniqueCxStrings) {
    std::set<std::string> seen;
    for (MemoryErrorCode c : kAll) {
        std::string s = MemoryErrorCodeString(c);
        EXPECT_FALSE(s.empty());
        EXPECT_EQ(s.rfind("CX_ERR_MEMORY_", 0), 0u) << s << " must start with CX_ERR_MEMORY_";
        EXPECT_TRUE(seen.insert(s).second) << "duplicate code string: " << s;
    }
    EXPECT_EQ(seen.size(), 5u);
}

// §4.3.4.bis table, row by row.
TEST(MemoryErrorTest, RegistryMatchesSpecTable) {
    auto chk = [](MemoryErrorCode c, const char* code, int http, ErrorCategory cat,
                  bool retry, std::optional<int> retry_ms) {
        const MemoryErrorInfo& i = GetMemoryErrorInfo(c);
        EXPECT_STREQ(i.cx_code, code);
        EXPECT_EQ(i.http_status, http) << code;
        EXPECT_EQ(i.category, cat) << code;
        EXPECT_EQ(i.retryable, retry) << code;
        EXPECT_EQ(i.retry_after_ms, retry_ms) << code;
    };
    chk(MemoryErrorCode::kMemoryNotFound, "CX_ERR_MEMORY_NOT_FOUND", 404,
        ErrorCategory::kPermanent, false, std::nullopt);
    chk(MemoryErrorCode::kUserMismatch, "CX_ERR_MEMORY_USER_MISMATCH", 403,
        ErrorCategory::kAuth, false, std::nullopt);
    chk(MemoryErrorCode::kAlreadyInvalidated, "CX_ERR_MEMORY_ALREADY_INVALIDATED", 410,
        ErrorCategory::kPermanent, false, std::nullopt);
    chk(MemoryErrorCode::kInvalidateFailed, "CX_ERR_MEMORY_INVALIDATE_FAILED", 500,
        ErrorCategory::kTransient, true, 5000);
    chk(MemoryErrorCode::kQuota, "CX_ERR_MEMORY_QUOTA", 429,
        ErrorCategory::kQuota, true, 60000);
}

TEST(MemoryErrorTest, RetryableImpliesRetryAfterMs) {
    // GEN-Agent #6: a retryable error carries a machine-readable retry hint; a
    // non-retryable one carries none.
    for (MemoryErrorCode c : kAll) {
        const MemoryErrorInfo& i = GetMemoryErrorInfo(c);
        if (i.retryable) {
            EXPECT_TRUE(i.retry_after_ms.has_value()) << i.cx_code;
            EXPECT_GT(*i.retry_after_ms, 0) << i.cx_code;
        } else {
            EXPECT_FALSE(i.retry_after_ms.has_value()) << i.cx_code;
        }
    }
}

TEST(MemoryErrorTest, HttpStatusAccessor) {
    EXPECT_EQ(MemoryErrorHttpStatus(MemoryErrorCode::kMemoryNotFound), 404);
    EXPECT_EQ(MemoryErrorHttpStatus(MemoryErrorCode::kUserMismatch), 403);
    EXPECT_EQ(MemoryErrorHttpStatus(MemoryErrorCode::kAlreadyInvalidated), 410);
    EXPECT_EQ(MemoryErrorHttpStatus(MemoryErrorCode::kInvalidateFailed), 500);
    EXPECT_EQ(MemoryErrorHttpStatus(MemoryErrorCode::kQuota), 429);
}

TEST(MemoryErrorTest, RequiredStructuredDataKeysPerSpec) {
    EXPECT_EQ(RequiredStructuredDataKeys(MemoryErrorCode::kMemoryNotFound),
              (std::vector<std::string>{"memory_id"}));
    EXPECT_EQ(RequiredStructuredDataKeys(MemoryErrorCode::kUserMismatch),
              (std::vector<std::string>{"caller_user_id", "owner_user_id_masked"}));
    EXPECT_EQ(RequiredStructuredDataKeys(MemoryErrorCode::kAlreadyInvalidated),
              (std::vector<std::string>{"memory_id", "revoked_at", "deleted_by_user_id"}));
    EXPECT_EQ(RequiredStructuredDataKeys(MemoryErrorCode::kInvalidateFailed),
              (std::vector<std::string>{"memory_id", "failure_stage"}));
    EXPECT_EQ(RequiredStructuredDataKeys(MemoryErrorCode::kQuota),
              (std::vector<std::string>{"user_id", "namespace", "quota_used",
                                        "quota_limit", "window_seconds"}));
}

TEST(MemoryErrorTest, HasRequiredStructuredDataValidatesKeys) {
    nlohmann::json full = {{"memory_id", "mem_abc123"}, {"failure_stage", "metadata_update"}};
    EXPECT_TRUE(HasRequiredStructuredData(MemoryErrorCode::kInvalidateFailed, full));

    nlohmann::json missing = {{"memory_id", "mem_abc123"}};
    EXPECT_FALSE(HasRequiredStructuredData(MemoryErrorCode::kInvalidateFailed, missing));

    // Non-object payload only passes when no keys are required (all 5 require keys).
    EXPECT_FALSE(HasRequiredStructuredData(MemoryErrorCode::kMemoryNotFound,
                                           nlohmann::json("not-an-object")));
}

TEST(MemoryErrorTest, MakeMemoryErrorFillsFromRegistry) {
    nlohmann::json sd = {{"memory_id", "mem_abc123"}, {"failure_stage", "metadata_update"}};
    auto err = MakeMemoryError(MemoryErrorCode::kInvalidateFailed, sd,
                              "metadata_json update failed");
    EXPECT_EQ(err.code, "CX_ERR_MEMORY_INVALIDATE_FAILED");
    EXPECT_EQ(err.message, "metadata_json update failed");
    EXPECT_TRUE(err.retryable);
    EXPECT_EQ(err.category, ErrorCategory::kTransient);
    ASSERT_TRUE(err.retry_after_ms.has_value());
    EXPECT_EQ(*err.retry_after_ms, 5000);
    ASSERT_TRUE(err.structured_data.has_value());
    EXPECT_EQ((*err.structured_data)["memory_id"], "mem_abc123");
}

TEST(MemoryErrorTest, MakeMemoryErrorDefaultsMessageToCode) {
    auto err = MakeMemoryError(MemoryErrorCode::kMemoryNotFound);
    EXPECT_EQ(err.message, "CX_ERR_MEMORY_NOT_FOUND");
    EXPECT_FALSE(err.retryable);
    EXPECT_FALSE(err.retry_after_ms.has_value());
}

TEST(MemoryErrorTest, QuotaErrorIsRetryableQuotaCategory) {
    auto err = MakeMemoryError(MemoryErrorCode::kQuota,
                              {{"user_id", "user_x"}, {"namespace", "ns_default"},
                               {"quota_used", 100}, {"quota_limit", 100},
                               {"window_seconds", 60}});
    nlohmann::json j = agent_friendly::ToJson(err);
    EXPECT_EQ(j["code"], "CX_ERR_MEMORY_QUOTA");
    EXPECT_EQ(j["retryable"], true);
    EXPECT_EQ(j["category"], "quota");
    EXPECT_EQ(j["retry_after_ms"], 60000);
    EXPECT_EQ(j["structured_data"]["quota_limit"], 100);
}

TEST(MemoryErrorTest, ToJsonSerializesNotFoundBody) {
    auto err = MakeMemoryError(MemoryErrorCode::kMemoryNotFound, {{"memory_id", "mem_002"}});
    nlohmann::json j = agent_friendly::ToJson(err);
    EXPECT_EQ(j["code"], "CX_ERR_MEMORY_NOT_FOUND");
    EXPECT_EQ(j["retryable"], false);
    EXPECT_EQ(j["category"], "permanent");
    EXPECT_TRUE(j["retry_after_ms"].is_null());
    EXPECT_EQ(j["structured_data"]["memory_id"], "mem_002");
}

TEST(MemoryErrorTest, StatusBridgeCarriesCodeToken) {
    Status s = MemoryStatus(MemoryErrorCode::kUserMismatch, "caller != owner");
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kPermissionDenied);
    EXPECT_NE(s.message().find("CX_ERR_MEMORY_USER_MISMATCH"), std::string::npos);
    EXPECT_NE(s.message().find("caller != owner"), std::string::npos);
}

TEST(MemoryErrorTest, StatusCodeMappingIsTotalAndSane) {
    for (MemoryErrorCode c : kAll) {
        StatusCode sc = MemoryErrorToStatusCode(c);
        EXPECT_NE(sc, StatusCode::kOk) << MemoryErrorCodeString(c);
    }
    EXPECT_EQ(MemoryErrorToStatusCode(MemoryErrorCode::kMemoryNotFound),
              StatusCode::kNotFound);
    EXPECT_EQ(MemoryErrorToStatusCode(MemoryErrorCode::kUserMismatch),
              StatusCode::kPermissionDenied);
    EXPECT_EQ(MemoryErrorToStatusCode(MemoryErrorCode::kAlreadyInvalidated),
              StatusCode::kAlreadyExists);
    EXPECT_EQ(MemoryErrorToStatusCode(MemoryErrorCode::kInvalidateFailed),
              StatusCode::kInternal);
    EXPECT_EQ(MemoryErrorToStatusCode(MemoryErrorCode::kQuota),
              StatusCode::kUnavailable);
}

TEST(MemoryErrorTest, StatusBridgeDefaultsMessageToCodeOnly) {
    Status s = MemoryStatus(MemoryErrorCode::kAlreadyInvalidated);
    EXPECT_EQ(s.message(), "CX_ERR_MEMORY_ALREADY_INVALIDATED");
}

}  // namespace
}  // namespace cortrix::memory::transparency
