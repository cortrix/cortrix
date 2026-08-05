#include <gtest/gtest.h>

#include <set>
#include <string>

#include "cortrix/async/task_error.h"

// S1 coverage: the async task error model (template A) — all 11 CX_ERR_* identities, their
// §6.2 attributes (http/category/retryable/retry_after_ms/structured_data keys),
// the AgentFriendlyError builder, and the Status bridge.
namespace cortrix::async {
namespace {

using agent_friendly::ErrorCategory;

// Every enum value the suite walks (kept in sync with kTaskErrorCodeCount).
constexpr TaskErrorCode kAll[] = {
    TaskErrorCode::kInvalidRequest,          TaskErrorCode::kMaxPagesExceeded,
    TaskErrorCode::kTaskNotFound,            TaskErrorCode::kTaskTimeout,
    TaskErrorCode::kDocProcessingInProgress, TaskErrorCode::kTaskCancelling,
    TaskErrorCode::kParseFailed,             TaskErrorCode::kStorageFailed,
    TaskErrorCode::kWorkerBusy,              TaskErrorCode::kZombieTaskCleanup,
    TaskErrorCode::kServiceUnavailable,
};

TEST(TaskErrorTest, CountMatchesEnumeration) {
    EXPECT_EQ(kTaskErrorCodeCount, 11);
    EXPECT_EQ(std::size(kAll), static_cast<size_t>(kTaskErrorCodeCount));
}

TEST(TaskErrorTest, AllCodesHaveUniqueCxStrings) {
    std::set<std::string> seen;
    for (TaskErrorCode c : kAll) {
        std::string s = TaskErrorCodeString(c);
        EXPECT_FALSE(s.empty());
        EXPECT_EQ(s.rfind("CX_ERR_", 0), 0u) << s << " must start with CX_ERR_";
        EXPECT_TRUE(seen.insert(s).second) << "duplicate code string: " << s;
    }
    EXPECT_EQ(seen.size(), 11u);
}

// §6.2 table, row by row.
TEST(TaskErrorTest, RegistryMatchesSpecTable) {
    auto chk = [](TaskErrorCode c, const char* code, int http, ErrorCategory cat,
                  bool retry, std::optional<int> retry_ms) {
        const TaskErrorInfo& i = GetTaskErrorInfo(c);
        EXPECT_STREQ(i.cx_code, code);
        EXPECT_EQ(i.http_status, http) << code;
        EXPECT_EQ(i.category, cat) << code;
        EXPECT_EQ(i.retryable, retry) << code;
        EXPECT_EQ(i.retry_after_ms, retry_ms) << code;
    };
    chk(TaskErrorCode::kInvalidRequest, "CX_ERR_INVALID_REQUEST", 400,
        ErrorCategory::kPermanent, false, std::nullopt);
    chk(TaskErrorCode::kMaxPagesExceeded, "CX_ERR_MAX_PAGES_EXCEEDED", 403,
        ErrorCategory::kPermanent, false, std::nullopt);
    chk(TaskErrorCode::kTaskNotFound, "CX_ERR_TASK_NOT_FOUND", 404,
        ErrorCategory::kPermanent, false, std::nullopt);
    chk(TaskErrorCode::kTaskTimeout, "CX_ERR_TASK_TIMEOUT", 408,
        ErrorCategory::kTimeout, true, 60000);
    chk(TaskErrorCode::kDocProcessingInProgress, "CX_ERR_DOC_PROCESSING_IN_PROGRESS",
        409, ErrorCategory::kTransient, true, 30000);
    chk(TaskErrorCode::kTaskCancelling, "CX_ERR_TASK_CANCELLING", 423,
        ErrorCategory::kPermanent, false, std::nullopt);
    chk(TaskErrorCode::kParseFailed, "CX_ERR_PARSE_FAILED", 500,
        ErrorCategory::kPermanent, false, std::nullopt);
    chk(TaskErrorCode::kStorageFailed, "CX_ERR_STORAGE_FAILED", 500,
        ErrorCategory::kPermanent, false, std::nullopt);
    chk(TaskErrorCode::kWorkerBusy, "CX_ERR_WORKER_BUSY", 500,
        ErrorCategory::kTransient, true, 10000);
    chk(TaskErrorCode::kZombieTaskCleanup, "CX_ERR_ZOMBIE_TASK_CLEANUP", 500,
        ErrorCategory::kPermanent, false, std::nullopt);
    chk(TaskErrorCode::kServiceUnavailable, "CX_ERR_SERVICE_UNAVAILABLE", 503,
        ErrorCategory::kTransient, true, 10000);
}

TEST(TaskErrorTest, RetryableImpliesRetryAfterMs) {
    // GEN-Agent #6: a retryable error carries a machine-readable retry hint; a
    // non-retryable one carries none.
    for (TaskErrorCode c : kAll) {
        const TaskErrorInfo& i = GetTaskErrorInfo(c);
        if (i.retryable) {
            EXPECT_TRUE(i.retry_after_ms.has_value()) << i.cx_code;
            EXPECT_GT(*i.retry_after_ms, 0) << i.cx_code;
        } else {
            EXPECT_FALSE(i.retry_after_ms.has_value()) << i.cx_code;
        }
    }
}

TEST(TaskErrorTest, HttpStatusAccessor) {
    EXPECT_EQ(TaskErrorHttpStatus(TaskErrorCode::kMaxPagesExceeded), 403);
    EXPECT_EQ(TaskErrorHttpStatus(TaskErrorCode::kTaskNotFound), 404);
    EXPECT_EQ(TaskErrorHttpStatus(TaskErrorCode::kServiceUnavailable), 503);
}

TEST(TaskErrorTest, RequiredStructuredDataKeysPerSpec) {
    EXPECT_EQ(RequiredStructuredDataKeys(TaskErrorCode::kMaxPagesExceeded),
              (std::vector<std::string>{"max_pages", "actual_pages", "edition"}));
    // TASK_NOT_FOUND → {} (no required keys).
    EXPECT_TRUE(RequiredStructuredDataKeys(TaskErrorCode::kTaskNotFound).empty());
    EXPECT_EQ(RequiredStructuredDataKeys(TaskErrorCode::kServiceUnavailable),
              (std::vector<std::string>{"component"}));
    EXPECT_EQ(RequiredStructuredDataKeys(TaskErrorCode::kWorkerBusy),
              (std::vector<std::string>{"active_tasks", "max_workers"}));
}

TEST(TaskErrorTest, HasRequiredStructuredDataValidatesKeys) {
    nlohmann::json full = {{"max_pages", 200}, {"actual_pages", 800}, {"edition", "CE"}};
    EXPECT_TRUE(HasRequiredStructuredData(TaskErrorCode::kMaxPagesExceeded, full));

    nlohmann::json missing = {{"max_pages", 200}};
    EXPECT_FALSE(HasRequiredStructuredData(TaskErrorCode::kMaxPagesExceeded, missing));

    // A code with no required keys accepts an empty object.
    EXPECT_TRUE(HasRequiredStructuredData(TaskErrorCode::kTaskNotFound,
                                          nlohmann::json::object()));
    // Non-object payload only passes when no keys are required.
    EXPECT_FALSE(HasRequiredStructuredData(TaskErrorCode::kMaxPagesExceeded,
                                           nlohmann::json("not-an-object")));
    EXPECT_TRUE(HasRequiredStructuredData(TaskErrorCode::kTaskNotFound,
                                          nlohmann::json("anything")));
}

TEST(TaskErrorTest, MakeTaskErrorFillsFromRegistry) {
    nlohmann::json sd = {{"active_tasks", 2}, {"max_workers", 2}};
    auto err = MakeTaskError(TaskErrorCode::kWorkerBusy, sd, "all workers busy");
    EXPECT_EQ(err.code, "CX_ERR_WORKER_BUSY");
    EXPECT_EQ(err.message, "all workers busy");
    EXPECT_TRUE(err.retryable);
    EXPECT_EQ(err.category, ErrorCategory::kTransient);
    ASSERT_TRUE(err.retry_after_ms.has_value());
    EXPECT_EQ(*err.retry_after_ms, 10000);
    ASSERT_TRUE(err.structured_data.has_value());
    EXPECT_EQ((*err.structured_data)["active_tasks"], 2);
}

TEST(TaskErrorTest, MakeTaskErrorDefaultsMessageToCode) {
    auto err = MakeTaskError(TaskErrorCode::kTaskNotFound);
    EXPECT_EQ(err.message, "CX_ERR_TASK_NOT_FOUND");
    EXPECT_FALSE(err.retryable);
    EXPECT_FALSE(err.retry_after_ms.has_value());
}

TEST(TaskErrorTest, ToJsonSerializesAgentFriendlyBody) {
    auto err = MakeTaskError(TaskErrorCode::kTaskTimeout,
                            {{"task_id", "t1"}, {"timeout_seconds", 1800}, {"last_phase", "parsing"}});
    nlohmann::json j = agent_friendly::ToJson(err);
    EXPECT_EQ(j["code"], "CX_ERR_TASK_TIMEOUT");
    EXPECT_EQ(j["retryable"], true);
    EXPECT_EQ(j["category"], "timeout");
    EXPECT_EQ(j["retry_after_ms"], 60000);
    EXPECT_EQ(j["structured_data"]["timeout_seconds"], 1800);
}

TEST(TaskErrorTest, StatusBridgeCarriesCodeToken) {
    Status s = TaskStatus(TaskErrorCode::kTaskNotFound, "task-xyz");
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kNotFound);
    EXPECT_NE(s.message().find("CX_ERR_TASK_NOT_FOUND"), std::string::npos);
    EXPECT_NE(s.message().find("task-xyz"), std::string::npos);
}

TEST(TaskErrorTest, StatusCodeMappingIsTotalAndSane) {
    for (TaskErrorCode c : kAll) {
        StatusCode sc = TaskErrorToStatusCode(c);
        EXPECT_NE(sc, StatusCode::kOk) << TaskErrorCodeString(c);
    }
    EXPECT_EQ(TaskErrorToStatusCode(TaskErrorCode::kInvalidRequest),
              StatusCode::kInvalidArgument);
    EXPECT_EQ(TaskErrorToStatusCode(TaskErrorCode::kTaskNotFound), StatusCode::kNotFound);
    EXPECT_EQ(TaskErrorToStatusCode(TaskErrorCode::kWorkerBusy), StatusCode::kUnavailable);
    EXPECT_EQ(TaskErrorToStatusCode(TaskErrorCode::kParseFailed), StatusCode::kInternal);
}

}  // namespace
}  // namespace cortrix::async
