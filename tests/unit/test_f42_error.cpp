#include <gtest/gtest.h>

#include <set>
#include <string>

#include "cortrix/async/f42_error.h"

// S1 coverage: the F42 error model (template A) — all 11 CX_ERR_* identities, their
// §6.2 attributes (http/category/retryable/retry_after_ms/structured_data keys),
// the AgentFriendlyError builder, and the Status bridge.
namespace cortrix::async {
namespace {

using agent_friendly::ErrorCategory;

// Every enum value the suite walks (kept in sync with kF42ErrorCodeCount).
constexpr F42ErrorCode kAll[] = {
    F42ErrorCode::kInvalidRequest,          F42ErrorCode::kMaxPagesExceeded,
    F42ErrorCode::kTaskNotFound,            F42ErrorCode::kTaskTimeout,
    F42ErrorCode::kDocProcessingInProgress, F42ErrorCode::kTaskCancelling,
    F42ErrorCode::kParseFailed,             F42ErrorCode::kStorageFailed,
    F42ErrorCode::kWorkerBusy,              F42ErrorCode::kZombieTaskCleanup,
    F42ErrorCode::kServiceUnavailable,
};

TEST(F42ErrorTest, CountMatchesEnumeration) {
    EXPECT_EQ(kF42ErrorCodeCount, 11);
    EXPECT_EQ(std::size(kAll), static_cast<size_t>(kF42ErrorCodeCount));
}

TEST(F42ErrorTest, AllCodesHaveUniqueCxStrings) {
    std::set<std::string> seen;
    for (F42ErrorCode c : kAll) {
        std::string s = F42ErrorCodeString(c);
        EXPECT_FALSE(s.empty());
        EXPECT_EQ(s.rfind("CX_ERR_", 0), 0u) << s << " must start with CX_ERR_";
        EXPECT_TRUE(seen.insert(s).second) << "duplicate code string: " << s;
    }
    EXPECT_EQ(seen.size(), 11u);
}

// §6.2 table, row by row.
TEST(F42ErrorTest, RegistryMatchesSpecTable) {
    auto chk = [](F42ErrorCode c, const char* code, int http, ErrorCategory cat,
                  bool retry, std::optional<int> retry_ms) {
        const F42ErrorInfo& i = GetF42ErrorInfo(c);
        EXPECT_STREQ(i.cx_code, code);
        EXPECT_EQ(i.http_status, http) << code;
        EXPECT_EQ(i.category, cat) << code;
        EXPECT_EQ(i.retryable, retry) << code;
        EXPECT_EQ(i.retry_after_ms, retry_ms) << code;
    };
    chk(F42ErrorCode::kInvalidRequest, "CX_ERR_INVALID_REQUEST", 400,
        ErrorCategory::kPermanent, false, std::nullopt);
    chk(F42ErrorCode::kMaxPagesExceeded, "CX_ERR_MAX_PAGES_EXCEEDED", 403,
        ErrorCategory::kPermanent, false, std::nullopt);
    chk(F42ErrorCode::kTaskNotFound, "CX_ERR_TASK_NOT_FOUND", 404,
        ErrorCategory::kPermanent, false, std::nullopt);
    chk(F42ErrorCode::kTaskTimeout, "CX_ERR_TASK_TIMEOUT", 408,
        ErrorCategory::kTimeout, true, 60000);
    chk(F42ErrorCode::kDocProcessingInProgress, "CX_ERR_DOC_PROCESSING_IN_PROGRESS",
        409, ErrorCategory::kTransient, true, 30000);
    chk(F42ErrorCode::kTaskCancelling, "CX_ERR_TASK_CANCELLING", 423,
        ErrorCategory::kPermanent, false, std::nullopt);
    chk(F42ErrorCode::kParseFailed, "CX_ERR_PARSE_FAILED", 500,
        ErrorCategory::kPermanent, false, std::nullopt);
    chk(F42ErrorCode::kStorageFailed, "CX_ERR_STORAGE_FAILED", 500,
        ErrorCategory::kPermanent, false, std::nullopt);
    chk(F42ErrorCode::kWorkerBusy, "CX_ERR_WORKER_BUSY", 500,
        ErrorCategory::kTransient, true, 10000);
    chk(F42ErrorCode::kZombieTaskCleanup, "CX_ERR_ZOMBIE_TASK_CLEANUP", 500,
        ErrorCategory::kPermanent, false, std::nullopt);
    chk(F42ErrorCode::kServiceUnavailable, "CX_ERR_SERVICE_UNAVAILABLE", 503,
        ErrorCategory::kTransient, true, 10000);
}

TEST(F42ErrorTest, RetryableImpliesRetryAfterMs) {
    // GEN-Agent #6: a retryable error carries a machine-readable retry hint; a
    // non-retryable one carries none.
    for (F42ErrorCode c : kAll) {
        const F42ErrorInfo& i = GetF42ErrorInfo(c);
        if (i.retryable) {
            EXPECT_TRUE(i.retry_after_ms.has_value()) << i.cx_code;
            EXPECT_GT(*i.retry_after_ms, 0) << i.cx_code;
        } else {
            EXPECT_FALSE(i.retry_after_ms.has_value()) << i.cx_code;
        }
    }
}

TEST(F42ErrorTest, HttpStatusAccessor) {
    EXPECT_EQ(F42ErrorHttpStatus(F42ErrorCode::kMaxPagesExceeded), 403);
    EXPECT_EQ(F42ErrorHttpStatus(F42ErrorCode::kTaskNotFound), 404);
    EXPECT_EQ(F42ErrorHttpStatus(F42ErrorCode::kServiceUnavailable), 503);
}

TEST(F42ErrorTest, RequiredStructuredDataKeysPerSpec) {
    EXPECT_EQ(RequiredStructuredDataKeys(F42ErrorCode::kMaxPagesExceeded),
              (std::vector<std::string>{"max_pages", "actual_pages", "edition"}));
    // TASK_NOT_FOUND → {} (no required keys).
    EXPECT_TRUE(RequiredStructuredDataKeys(F42ErrorCode::kTaskNotFound).empty());
    EXPECT_EQ(RequiredStructuredDataKeys(F42ErrorCode::kServiceUnavailable),
              (std::vector<std::string>{"component"}));
    EXPECT_EQ(RequiredStructuredDataKeys(F42ErrorCode::kWorkerBusy),
              (std::vector<std::string>{"active_tasks", "max_workers"}));
}

TEST(F42ErrorTest, HasRequiredStructuredDataValidatesKeys) {
    nlohmann::json full = {{"max_pages", 200}, {"actual_pages", 800}, {"edition", "CE"}};
    EXPECT_TRUE(HasRequiredStructuredData(F42ErrorCode::kMaxPagesExceeded, full));

    nlohmann::json missing = {{"max_pages", 200}};
    EXPECT_FALSE(HasRequiredStructuredData(F42ErrorCode::kMaxPagesExceeded, missing));

    // A code with no required keys accepts an empty object.
    EXPECT_TRUE(HasRequiredStructuredData(F42ErrorCode::kTaskNotFound,
                                          nlohmann::json::object()));
    // Non-object payload only passes when no keys are required.
    EXPECT_FALSE(HasRequiredStructuredData(F42ErrorCode::kMaxPagesExceeded,
                                           nlohmann::json("not-an-object")));
    EXPECT_TRUE(HasRequiredStructuredData(F42ErrorCode::kTaskNotFound,
                                          nlohmann::json("anything")));
}

TEST(F42ErrorTest, MakeF42ErrorFillsFromRegistry) {
    nlohmann::json sd = {{"active_tasks", 2}, {"max_workers", 2}};
    auto err = MakeF42Error(F42ErrorCode::kWorkerBusy, sd, "all workers busy");
    EXPECT_EQ(err.code, "CX_ERR_WORKER_BUSY");
    EXPECT_EQ(err.message, "all workers busy");
    EXPECT_TRUE(err.retryable);
    EXPECT_EQ(err.category, ErrorCategory::kTransient);
    ASSERT_TRUE(err.retry_after_ms.has_value());
    EXPECT_EQ(*err.retry_after_ms, 10000);
    ASSERT_TRUE(err.structured_data.has_value());
    EXPECT_EQ((*err.structured_data)["active_tasks"], 2);
}

TEST(F42ErrorTest, MakeF42ErrorDefaultsMessageToCode) {
    auto err = MakeF42Error(F42ErrorCode::kTaskNotFound);
    EXPECT_EQ(err.message, "CX_ERR_TASK_NOT_FOUND");
    EXPECT_FALSE(err.retryable);
    EXPECT_FALSE(err.retry_after_ms.has_value());
}

TEST(F42ErrorTest, ToJsonSerializesAgentFriendlyBody) {
    auto err = MakeF42Error(F42ErrorCode::kTaskTimeout,
                            {{"task_id", "t1"}, {"timeout_seconds", 1800}, {"last_phase", "parsing"}});
    nlohmann::json j = agent_friendly::ToJson(err);
    EXPECT_EQ(j["code"], "CX_ERR_TASK_TIMEOUT");
    EXPECT_EQ(j["retryable"], true);
    EXPECT_EQ(j["category"], "timeout");
    EXPECT_EQ(j["retry_after_ms"], 60000);
    EXPECT_EQ(j["structured_data"]["timeout_seconds"], 1800);
}

TEST(F42ErrorTest, StatusBridgeCarriesCodeToken) {
    Status s = F42Status(F42ErrorCode::kTaskNotFound, "task-xyz");
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kNotFound);
    EXPECT_NE(s.message().find("CX_ERR_TASK_NOT_FOUND"), std::string::npos);
    EXPECT_NE(s.message().find("task-xyz"), std::string::npos);
}

TEST(F42ErrorTest, StatusCodeMappingIsTotalAndSane) {
    for (F42ErrorCode c : kAll) {
        StatusCode sc = F42ErrorToStatusCode(c);
        EXPECT_NE(sc, StatusCode::kOk) << F42ErrorCodeString(c);
    }
    EXPECT_EQ(F42ErrorToStatusCode(F42ErrorCode::kInvalidRequest),
              StatusCode::kInvalidArgument);
    EXPECT_EQ(F42ErrorToStatusCode(F42ErrorCode::kTaskNotFound), StatusCode::kNotFound);
    EXPECT_EQ(F42ErrorToStatusCode(F42ErrorCode::kWorkerBusy), StatusCode::kUnavailable);
    EXPECT_EQ(F42ErrorToStatusCode(F42ErrorCode::kParseFailed), StatusCode::kInternal);
}

}  // namespace
}  // namespace cortrix::async
