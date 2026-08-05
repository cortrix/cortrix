#include <gtest/gtest.h>

#include <set>
#include <string>

#include "cortrix/memory/mem03_error.h"

// S5 coverage: the memory transparency error model (template A) — all 5 CX_ERR_MEM03_* identities,
// their §4.3.4.bis attributes (http/category/retryable/retry_after_ms/structured_data
// keys), the AgentFriendlyError builder, and the Status bridge.
namespace cortrix::memory::transparency {
namespace {

using agent_friendly::ErrorCategory;

// Every enum value the suite walks (kept in sync with kMem03ErrorCodeCount).
constexpr Mem03ErrorCode kAll[] = {
    Mem03ErrorCode::kMemoryNotFound,
    Mem03ErrorCode::kUserMismatch,
    Mem03ErrorCode::kAlreadyInvalidated,
    Mem03ErrorCode::kInvalidateFailed,
    Mem03ErrorCode::kQuota,
};

TEST(Mem03ErrorTest, CountMatchesEnumeration) {
    EXPECT_EQ(kMem03ErrorCodeCount, 5);
    EXPECT_EQ(std::size(kAll), static_cast<size_t>(kMem03ErrorCodeCount));
}

TEST(Mem03ErrorTest, AllCodesHaveUniqueCxStrings) {
    std::set<std::string> seen;
    for (Mem03ErrorCode c : kAll) {
        std::string s = Mem03ErrorCodeString(c);
        EXPECT_FALSE(s.empty());
        EXPECT_EQ(s.rfind("CX_ERR_MEM03_", 0), 0u) << s << " must start with CX_ERR_MEM03_";
        EXPECT_TRUE(seen.insert(s).second) << "duplicate code string: " << s;
    }
    EXPECT_EQ(seen.size(), 5u);
}

// §4.3.4.bis table, row by row.
TEST(Mem03ErrorTest, RegistryMatchesSpecTable) {
    auto chk = [](Mem03ErrorCode c, const char* code, int http, ErrorCategory cat,
                  bool retry, std::optional<int> retry_ms) {
        const Mem03ErrorInfo& i = GetMem03ErrorInfo(c);
        EXPECT_STREQ(i.cx_code, code);
        EXPECT_EQ(i.http_status, http) << code;
        EXPECT_EQ(i.category, cat) << code;
        EXPECT_EQ(i.retryable, retry) << code;
        EXPECT_EQ(i.retry_after_ms, retry_ms) << code;
    };
    chk(Mem03ErrorCode::kMemoryNotFound, "CX_ERR_MEM03_MEMORY_NOT_FOUND", 404,
        ErrorCategory::kPermanent, false, std::nullopt);
    chk(Mem03ErrorCode::kUserMismatch, "CX_ERR_MEM03_USER_MISMATCH", 403,
        ErrorCategory::kAuth, false, std::nullopt);
    chk(Mem03ErrorCode::kAlreadyInvalidated, "CX_ERR_MEM03_ALREADY_INVALIDATED", 410,
        ErrorCategory::kPermanent, false, std::nullopt);
    chk(Mem03ErrorCode::kInvalidateFailed, "CX_ERR_MEM03_INVALIDATE_FAILED", 500,
        ErrorCategory::kTransient, true, 5000);
    chk(Mem03ErrorCode::kQuota, "CX_ERR_MEM03_QUOTA", 429,
        ErrorCategory::kQuota, true, 60000);
}

TEST(Mem03ErrorTest, RetryableImpliesRetryAfterMs) {
    // GEN-Agent #6: a retryable error carries a machine-readable retry hint; a
    // non-retryable one carries none.
    for (Mem03ErrorCode c : kAll) {
        const Mem03ErrorInfo& i = GetMem03ErrorInfo(c);
        if (i.retryable) {
            EXPECT_TRUE(i.retry_after_ms.has_value()) << i.cx_code;
            EXPECT_GT(*i.retry_after_ms, 0) << i.cx_code;
        } else {
            EXPECT_FALSE(i.retry_after_ms.has_value()) << i.cx_code;
        }
    }
}

TEST(Mem03ErrorTest, HttpStatusAccessor) {
    EXPECT_EQ(Mem03ErrorHttpStatus(Mem03ErrorCode::kMemoryNotFound), 404);
    EXPECT_EQ(Mem03ErrorHttpStatus(Mem03ErrorCode::kUserMismatch), 403);
    EXPECT_EQ(Mem03ErrorHttpStatus(Mem03ErrorCode::kAlreadyInvalidated), 410);
    EXPECT_EQ(Mem03ErrorHttpStatus(Mem03ErrorCode::kInvalidateFailed), 500);
    EXPECT_EQ(Mem03ErrorHttpStatus(Mem03ErrorCode::kQuota), 429);
}

TEST(Mem03ErrorTest, RequiredStructuredDataKeysPerSpec) {
    EXPECT_EQ(RequiredStructuredDataKeys(Mem03ErrorCode::kMemoryNotFound),
              (std::vector<std::string>{"memory_id"}));
    EXPECT_EQ(RequiredStructuredDataKeys(Mem03ErrorCode::kUserMismatch),
              (std::vector<std::string>{"caller_user_id", "owner_user_id_masked"}));
    EXPECT_EQ(RequiredStructuredDataKeys(Mem03ErrorCode::kAlreadyInvalidated),
              (std::vector<std::string>{"memory_id", "revoked_at", "deleted_by_user_id"}));
    EXPECT_EQ(RequiredStructuredDataKeys(Mem03ErrorCode::kInvalidateFailed),
              (std::vector<std::string>{"memory_id", "failure_stage"}));
    EXPECT_EQ(RequiredStructuredDataKeys(Mem03ErrorCode::kQuota),
              (std::vector<std::string>{"user_id", "namespace", "quota_used",
                                        "quota_limit", "window_seconds"}));
}

TEST(Mem03ErrorTest, HasRequiredStructuredDataValidatesKeys) {
    nlohmann::json full = {{"memory_id", "mem_abc123"}, {"failure_stage", "metadata_update"}};
    EXPECT_TRUE(HasRequiredStructuredData(Mem03ErrorCode::kInvalidateFailed, full));

    nlohmann::json missing = {{"memory_id", "mem_abc123"}};
    EXPECT_FALSE(HasRequiredStructuredData(Mem03ErrorCode::kInvalidateFailed, missing));

    // Non-object payload only passes when no keys are required (all 5 require keys).
    EXPECT_FALSE(HasRequiredStructuredData(Mem03ErrorCode::kMemoryNotFound,
                                           nlohmann::json("not-an-object")));
}

TEST(Mem03ErrorTest, MakeMem03ErrorFillsFromRegistry) {
    nlohmann::json sd = {{"memory_id", "mem_abc123"}, {"failure_stage", "metadata_update"}};
    auto err = MakeMem03Error(Mem03ErrorCode::kInvalidateFailed, sd,
                              "metadata_json update failed");
    EXPECT_EQ(err.code, "CX_ERR_MEM03_INVALIDATE_FAILED");
    EXPECT_EQ(err.message, "metadata_json update failed");
    EXPECT_TRUE(err.retryable);
    EXPECT_EQ(err.category, ErrorCategory::kTransient);
    ASSERT_TRUE(err.retry_after_ms.has_value());
    EXPECT_EQ(*err.retry_after_ms, 5000);
    ASSERT_TRUE(err.structured_data.has_value());
    EXPECT_EQ((*err.structured_data)["memory_id"], "mem_abc123");
}

TEST(Mem03ErrorTest, MakeMem03ErrorDefaultsMessageToCode) {
    auto err = MakeMem03Error(Mem03ErrorCode::kMemoryNotFound);
    EXPECT_EQ(err.message, "CX_ERR_MEM03_MEMORY_NOT_FOUND");
    EXPECT_FALSE(err.retryable);
    EXPECT_FALSE(err.retry_after_ms.has_value());
}

TEST(Mem03ErrorTest, QuotaErrorIsRetryableQuotaCategory) {
    auto err = MakeMem03Error(Mem03ErrorCode::kQuota,
                              {{"user_id", "user_x"}, {"namespace", "ns_default"},
                               {"quota_used", 100}, {"quota_limit", 100},
                               {"window_seconds", 60}});
    nlohmann::json j = agent_friendly::ToJson(err);
    EXPECT_EQ(j["code"], "CX_ERR_MEM03_QUOTA");
    EXPECT_EQ(j["retryable"], true);
    EXPECT_EQ(j["category"], "quota");
    EXPECT_EQ(j["retry_after_ms"], 60000);
    EXPECT_EQ(j["structured_data"]["quota_limit"], 100);
}

TEST(Mem03ErrorTest, ToJsonSerializesNotFoundBody) {
    auto err = MakeMem03Error(Mem03ErrorCode::kMemoryNotFound, {{"memory_id", "mem_002"}});
    nlohmann::json j = agent_friendly::ToJson(err);
    EXPECT_EQ(j["code"], "CX_ERR_MEM03_MEMORY_NOT_FOUND");
    EXPECT_EQ(j["retryable"], false);
    EXPECT_EQ(j["category"], "permanent");
    EXPECT_TRUE(j["retry_after_ms"].is_null());
    EXPECT_EQ(j["structured_data"]["memory_id"], "mem_002");
}

TEST(Mem03ErrorTest, StatusBridgeCarriesCodeToken) {
    Status s = Mem03Status(Mem03ErrorCode::kUserMismatch, "caller != owner");
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kPermissionDenied);
    EXPECT_NE(s.message().find("CX_ERR_MEM03_USER_MISMATCH"), std::string::npos);
    EXPECT_NE(s.message().find("caller != owner"), std::string::npos);
}

TEST(Mem03ErrorTest, StatusCodeMappingIsTotalAndSane) {
    for (Mem03ErrorCode c : kAll) {
        StatusCode sc = Mem03ErrorToStatusCode(c);
        EXPECT_NE(sc, StatusCode::kOk) << Mem03ErrorCodeString(c);
    }
    EXPECT_EQ(Mem03ErrorToStatusCode(Mem03ErrorCode::kMemoryNotFound),
              StatusCode::kNotFound);
    EXPECT_EQ(Mem03ErrorToStatusCode(Mem03ErrorCode::kUserMismatch),
              StatusCode::kPermissionDenied);
    EXPECT_EQ(Mem03ErrorToStatusCode(Mem03ErrorCode::kAlreadyInvalidated),
              StatusCode::kAlreadyExists);
    EXPECT_EQ(Mem03ErrorToStatusCode(Mem03ErrorCode::kInvalidateFailed),
              StatusCode::kInternal);
    EXPECT_EQ(Mem03ErrorToStatusCode(Mem03ErrorCode::kQuota),
              StatusCode::kUnavailable);
}

TEST(Mem03ErrorTest, StatusBridgeDefaultsMessageToCodeOnly) {
    Status s = Mem03Status(Mem03ErrorCode::kAlreadyInvalidated);
    EXPECT_EQ(s.message(), "CX_ERR_MEM03_ALREADY_INVALIDATED");
}

}  // namespace
}  // namespace cortrix::memory::transparency
