#include <gtest/gtest.h>

#include <regex>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cortrix/agent_friendly/error.h"
#include "cortrix/import/import_error.h"
#include "cortrix/import/import_response.h"
#include "cortrix/import/import_types.h"

// S5 coverage: the 6 DB import error codes — CX_ERR_IMPORT_ identity, category
// mapping, retryability, structured_data contract, the GEN-Agent 4-field boundary
// factory, and the Agent-friendly response bodies. Mirrors the
// project reference test scatter/test_cross_ns_error.cpp.
namespace cortrix::import {
namespace {

using agent_friendly::ErrorCategory;

// All 6 codes, in enum order. Explicit (not a loop over ints) so the test itself
// documents the locked set and fails to compile if an enumerator is gone.
const std::vector<ImportErrorCode>& AllCodes() {
    static const std::vector<ImportErrorCode> codes = {
        ImportErrorCode::kConnectionFailed,
        ImportErrorCode::kAuthDenied,
        ImportErrorCode::kInvalidSql,
        ImportErrorCode::kTimeout,
        ImportErrorCode::kRowsLimitExceeded,
        ImportErrorCode::kCrossTenantRef,
        ImportErrorCode::kInternal,  // [R2-M5] appended
    };
    return codes;
}

TEST(ImportErrorTest, SevenCodesTotal) {
    // [R2-M5] 6 -> 7: kInternal appended (GEN-Agent #7 allows new codes; none removed).
    EXPECT_EQ(AllCodes().size(), 7u);
    EXPECT_EQ(kImportErrorCodeCount, 7);
}

// Every code's CX_ERR_IMPORT_* string is unique and matches the API spec ErrorResponseV1
// pattern (GEN-Agent #1 + #7 stable identity).
TEST(ImportErrorTest, EveryCodeHasUniqueWellFormedCxString) {
    static const std::regex kPattern("^CX_ERR_IMPORT_[A-Z][A-Z_]*$");
    std::set<std::string> seen;
    for (ImportErrorCode code : AllCodes()) {
        std::string cx = ImportErrorCodeString(code);
        EXPECT_TRUE(std::regex_match(cx, kPattern)) << cx;
        EXPECT_TRUE(seen.insert(cx).second) << "duplicate " << cx;
    }
    EXPECT_EQ(seen.size(), 7u);
}

// column values pinned exactly (HTTP / category / retryable).
TEST(ImportErrorTest, RegistryMatchesSpecTable) {
    auto info = [](ImportErrorCode c) { return GetImportErrorInfo(c); };

    EXPECT_EQ(info(ImportErrorCode::kConnectionFailed).http_status, 503);
    EXPECT_EQ(info(ImportErrorCode::kConnectionFailed).category, ErrorCategory::kTransient);
    EXPECT_TRUE(info(ImportErrorCode::kConnectionFailed).retryable);

    EXPECT_EQ(info(ImportErrorCode::kAuthDenied).http_status, 403);
    EXPECT_EQ(info(ImportErrorCode::kAuthDenied).category, ErrorCategory::kAuth);
    EXPECT_FALSE(info(ImportErrorCode::kAuthDenied).retryable);

    EXPECT_EQ(info(ImportErrorCode::kInvalidSql).http_status, 400);
    EXPECT_EQ(info(ImportErrorCode::kInvalidSql).category, ErrorCategory::kPermanent);
    EXPECT_FALSE(info(ImportErrorCode::kInvalidSql).retryable);

    EXPECT_EQ(info(ImportErrorCode::kTimeout).http_status, 504);
    EXPECT_EQ(info(ImportErrorCode::kTimeout).category, ErrorCategory::kTimeout);
    EXPECT_TRUE(info(ImportErrorCode::kTimeout).retryable);

    EXPECT_EQ(info(ImportErrorCode::kRowsLimitExceeded).http_status, 413);
    EXPECT_EQ(info(ImportErrorCode::kRowsLimitExceeded).category, ErrorCategory::kQuota);
    EXPECT_FALSE(info(ImportErrorCode::kRowsLimitExceeded).retryable);

    EXPECT_EQ(info(ImportErrorCode::kCrossTenantRef).http_status, 403);
    EXPECT_EQ(info(ImportErrorCode::kCrossTenantRef).category, ErrorCategory::kAuth);
    EXPECT_FALSE(info(ImportErrorCode::kCrossTenantRef).retryable);

    // [R2-M5] kInternal: 500 transient, retryable with a short backoff.
    EXPECT_EQ(info(ImportErrorCode::kInternal).http_status, 500);
    EXPECT_EQ(info(ImportErrorCode::kInternal).category, ErrorCategory::kTransient);
    EXPECT_TRUE(info(ImportErrorCode::kInternal).retryable);
}

// retry_after_ms is present iff retryable (GEN-Agent #6 — machine-readable retry).
TEST(ImportErrorTest, RetryAfterMsConsistentWithRetryable) {
    for (ImportErrorCode code : AllCodes()) {
        const ImportErrorInfo& info = GetImportErrorInfo(code);
        EXPECT_EQ(info.retryable, info.retry_after_ms.has_value())
            << ImportErrorCodeString(code);
    }
}

// MakeImportError fills the 4 GEN-Agent fields from the registry (call sites never
// restate category/retryable/retry_after_ms).
TEST(ImportErrorTest, MakeErrorPopulatesAgentFriendlyFields) {
    auto err = MakeImportError(ImportErrorCode::kTimeout,
                             {{"task_id", "import_x"},
                              {"timeout_seconds", 300},
                              {"rows_imported_before_timeout", 4500}},
                             "Query exceeded 300s timeout on table users");
    EXPECT_EQ(err.code, "CX_ERR_IMPORT_TIMEOUT");
    EXPECT_TRUE(err.retryable);
    EXPECT_EQ(err.category, ErrorCategory::kTimeout);
    ASSERT_TRUE(err.retry_after_ms.has_value());
    EXPECT_EQ(*err.retry_after_ms, 30000);
    ASSERT_TRUE(err.structured_data.has_value());
    EXPECT_EQ((*err.structured_data)["timeout_seconds"], 300);
}

// Empty message falls back to the CX_ERR_ token (never an empty string).
TEST(ImportErrorTest, EmptyMessageFallsBackToCode) {
    auto err = MakeImportError(ImportErrorCode::kAuthDenied);
    EXPECT_EQ(err.message, "CX_ERR_IMPORT_AUTH_DENIED");
}

// Required structured_data keys (GEN-Agent #5) for the codes that declare them.
TEST(ImportErrorTest, RequiredStructuredDataContract) {
    EXPECT_TRUE(RequiredStructuredDataKeys(ImportErrorCode::kConnectionFailed).empty());
    EXPECT_TRUE(RequiredStructuredDataKeys(ImportErrorCode::kAuthDenied).empty());
    EXPECT_TRUE(RequiredStructuredDataKeys(ImportErrorCode::kInvalidSql).empty());

    nlohmann::json full = {{"task_id", "t"}, {"timeout_seconds", 300},
                           {"rows_imported_before_timeout", 1}};
    EXPECT_TRUE(HasRequiredStructuredData(ImportErrorCode::kTimeout, full));
    nlohmann::json partial = {{"task_id", "t"}};
    EXPECT_FALSE(HasRequiredStructuredData(ImportErrorCode::kTimeout, partial));

    nlohmann::json rows = {{"max_rows", 10000000}, {"estimated_rows", 12345678}};
    EXPECT_TRUE(HasRequiredStructuredData(ImportErrorCode::kRowsLimitExceeded, rows));

    nlohmann::json xt = {{"connection_ref", "db_conn_a"}, {"tenant_id", "t1"}};
    EXPECT_TRUE(HasRequiredStructuredData(ImportErrorCode::kCrossTenantRef, xt));
}

// ImportStatus bridges to a coarse Status whose message recovers the exact identity
// (F-FREEZE-1: no Result<T,E>; identity travels in the CX_ERR_ prefix).
TEST(ImportErrorTest, StatusBridgePreservesIdentityInMessage) {
    Status s = ImportStatus(ImportErrorCode::kCrossTenantRef, "ref db_conn_a not in tenant t1");
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kPermissionDenied);
    EXPECT_NE(s.message().find("CX_ERR_IMPORT_CROSS_TENANT_REF"), std::string::npos);

    EXPECT_EQ(ImportStatus(ImportErrorCode::kInvalidSql).code(), StatusCode::kInvalidArgument);
    EXPECT_EQ(ImportStatus(ImportErrorCode::kConnectionFailed).code(), StatusCode::kUnavailable);
    EXPECT_EQ(ImportStatus(ImportErrorCode::kTimeout).code(), StatusCode::kUnavailable);
    EXPECT_EQ(ImportStatus(ImportErrorCode::kRowsLimitExceeded).code(), StatusCode::kInvalidArgument);
}

// ImportException carries the full Agent-friendly error for the throw-to-handler path.
TEST(ImportErrorTest, ExceptionCarriesFullError) {
    try {
        throw ImportException(ImportErrorCode::kInvalidSql, nlohmann::json::object(),
                            "denied keyword UNION");
    } catch (const ImportException& e) {
        EXPECT_EQ(e.code(), ImportErrorCode::kInvalidSql);
        EXPECT_EQ(e.GetError().code, "CX_ERR_IMPORT_INVALID_SQL");
        EXPECT_EQ(e.GetError().message, "denied keyword UNION");
    }
}

// --- Agent-friendly response bodies ---

TEST(ImportResponseTest, ImportStartedBodyMatchesSpec) {
    ImportTaskProgress task;
    task.task_id = "import_xyz";
    task.status = ImportTaskStatus::kQueued;
    task.namespace_id = "customer_kb";
    task.queued_at = std::chrono::system_clock::now();

    auto body = BuildImportStartedResponse(task, "db_conn_abc", 12345);
    EXPECT_EQ(body["task_id"], "import_xyz");
    EXPECT_EQ(body["status"], "queued");
    EXPECT_EQ(body["namespace"], "customer_kb");
    EXPECT_EQ(body["connection_ref"], "db_conn_abc");
    EXPECT_EQ(body["estimated_rows"], 12345);
    EXPECT_EQ(body["progress_endpoint"], "/api/v1/import/tasks/import_xyz/progress");
    EXPECT_EQ(body["cancel_endpoint"], "/api/v1/import/tasks/import_xyz");
    EXPECT_TRUE(body["error"].is_null());
    // ISO-8601 UTC shape
    EXPECT_TRUE(std::regex_match(body["queued_at"].get<std::string>(),
                                 std::regex("^\\d{4}-\\d{2}-\\d{2}T\\d{2}:\\d{2}:\\d{2}Z$")));
}

TEST(ImportResponseTest, ProgressBodyRunningHasNullError) {
    ImportTaskProgress task;
    task.task_id = "import_xyz";
    task.status = ImportTaskStatus::kRunning;
    task.progress = 0.45;
    task.rows_imported = 5500;
    task.rows_total = 12345;
    task.started_at = std::chrono::system_clock::now();
    task.estimated_completion_at = std::chrono::system_clock::now();

    auto body = BuildProgressResponse(task);
    EXPECT_EQ(body["status"], "running");
    EXPECT_DOUBLE_EQ(body["progress"].get<double>(), 0.45);
    EXPECT_EQ(body["rows_imported"], 5500);
    EXPECT_TRUE(body["error"].is_null());
    EXPECT_TRUE(body["started_at"].is_string());
}

TEST(ImportResponseTest, ProgressBodyFailedEmbedsErrorBody) {
    ImportTaskProgress task;
    task.task_id = "import_xyz";
    task.status = ImportTaskStatus::kFailed;
    task.error = MakeImportError(ImportErrorCode::kTimeout,
                               {{"task_id", "import_xyz"},
                                {"timeout_seconds", 300},
                                {"rows_imported_before_timeout", 4500}},
                               "Query exceeded 300s timeout on table users");

    auto body = BuildProgressResponse(task);
    EXPECT_EQ(body["status"], "failed");
    ASSERT_TRUE(body["error"].is_object());
    EXPECT_EQ(body["error"]["code"], "CX_ERR_IMPORT_TIMEOUT");
    EXPECT_EQ(body["error"]["retryable"], true);
    EXPECT_EQ(body["error"]["category"], "timeout");
    EXPECT_EQ(body["error"]["retry_after_ms"], 30000);
}

// Cancelled is a clean terminal state, NOT an error (note).
TEST(ImportResponseTest, CancelledHasNoErrorBody) {
    ImportTaskProgress task;
    task.task_id = "import_xyz";
    task.status = ImportTaskStatus::kCancelled;
    auto body = BuildProgressResponse(task);
    EXPECT_EQ(body["status"], "cancelled");
    EXPECT_TRUE(body["error"].is_null());
}

// text_strategy wire parsing: per_row/merge accepted, template rejected (V1.0).
TEST(ImportTypesTest, ParseTextStrategyRejectsTemplate) {
    EXPECT_EQ(ParseTextStrategy("per_row"), TextStrategy::kPerRow);
    EXPECT_EQ(ParseTextStrategy("merge"), TextStrategy::kMerge);
    EXPECT_FALSE(ParseTextStrategy("template").has_value());
    EXPECT_FALSE(ParseTextStrategy("").has_value());
    EXPECT_STREQ(ToString(TextStrategy::kPerRow), "per_row");
    EXPECT_STREQ(ToString(TextStrategy::kMerge), "merge");
}

}  // namespace
}  // namespace cortrix::import
