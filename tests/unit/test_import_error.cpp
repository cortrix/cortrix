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

// S5 coverage: the 6 F16a error codes (F16a §5.4) — CX_ERR_F16A_ identity, category
// mapping, retryability, structured_data contract, the GEN-Agent 4-field boundary
// factory, and the §5.1/§5.2/§5.3 Agent-friendly response bodies. Mirrors the
// project reference test scatter/test_cross_ns_error.cpp.
namespace cortrix::import {
namespace {

using agent_friendly::ErrorCategory;

// All 6 codes, in enum order. Explicit (not a loop over ints) so the test itself
// documents the locked set and fails to compile if an enumerator is gone.
const std::vector<F16aErrorCode>& AllCodes() {
    static const std::vector<F16aErrorCode> codes = {
        F16aErrorCode::kConnectionFailed,
        F16aErrorCode::kAuthDenied,
        F16aErrorCode::kInvalidSql,
        F16aErrorCode::kTimeout,
        F16aErrorCode::kRowsLimitExceeded,
        F16aErrorCode::kCrossTenantRef,
        F16aErrorCode::kInternal,  // [R2-M5] appended
    };
    return codes;
}

TEST(F16aErrorTest, SevenCodesTotal) {
    // [R2-M5] 6 -> 7: kInternal appended (GEN-Agent #7 allows new codes; none removed).
    EXPECT_EQ(AllCodes().size(), 7u);
    EXPECT_EQ(kF16aErrorCodeCount, 7);
}

// Every code's CX_ERR_F16A_* string is unique and matches the P04 ErrorResponseV1
// pattern (GEN-Agent #1 + #7 stable identity).
TEST(F16aErrorTest, EveryCodeHasUniqueWellFormedCxString) {
    static const std::regex kPattern("^CX_ERR_F16A_[A-Z][A-Z_]*$");
    std::set<std::string> seen;
    for (F16aErrorCode code : AllCodes()) {
        std::string cx = F16aErrorCodeString(code);
        EXPECT_TRUE(std::regex_match(cx, kPattern)) << cx;
        EXPECT_TRUE(seen.insert(cx).second) << "duplicate " << cx;
    }
    EXPECT_EQ(seen.size(), 7u);
}

// §5.4 column values pinned exactly (HTTP / category / retryable).
TEST(F16aErrorTest, RegistryMatchesSpecTable) {
    auto info = [](F16aErrorCode c) { return GetF16aErrorInfo(c); };

    EXPECT_EQ(info(F16aErrorCode::kConnectionFailed).http_status, 503);
    EXPECT_EQ(info(F16aErrorCode::kConnectionFailed).category, ErrorCategory::kTransient);
    EXPECT_TRUE(info(F16aErrorCode::kConnectionFailed).retryable);

    EXPECT_EQ(info(F16aErrorCode::kAuthDenied).http_status, 403);
    EXPECT_EQ(info(F16aErrorCode::kAuthDenied).category, ErrorCategory::kAuth);
    EXPECT_FALSE(info(F16aErrorCode::kAuthDenied).retryable);

    EXPECT_EQ(info(F16aErrorCode::kInvalidSql).http_status, 400);
    EXPECT_EQ(info(F16aErrorCode::kInvalidSql).category, ErrorCategory::kPermanent);
    EXPECT_FALSE(info(F16aErrorCode::kInvalidSql).retryable);

    EXPECT_EQ(info(F16aErrorCode::kTimeout).http_status, 504);
    EXPECT_EQ(info(F16aErrorCode::kTimeout).category, ErrorCategory::kTimeout);
    EXPECT_TRUE(info(F16aErrorCode::kTimeout).retryable);

    EXPECT_EQ(info(F16aErrorCode::kRowsLimitExceeded).http_status, 413);
    EXPECT_EQ(info(F16aErrorCode::kRowsLimitExceeded).category, ErrorCategory::kQuota);
    EXPECT_FALSE(info(F16aErrorCode::kRowsLimitExceeded).retryable);

    EXPECT_EQ(info(F16aErrorCode::kCrossTenantRef).http_status, 403);
    EXPECT_EQ(info(F16aErrorCode::kCrossTenantRef).category, ErrorCategory::kAuth);
    EXPECT_FALSE(info(F16aErrorCode::kCrossTenantRef).retryable);

    // [R2-M5] kInternal: 500 transient, retryable with a short backoff.
    EXPECT_EQ(info(F16aErrorCode::kInternal).http_status, 500);
    EXPECT_EQ(info(F16aErrorCode::kInternal).category, ErrorCategory::kTransient);
    EXPECT_TRUE(info(F16aErrorCode::kInternal).retryable);
}

// retry_after_ms is present iff retryable (GEN-Agent #6 — machine-readable retry).
TEST(F16aErrorTest, RetryAfterMsConsistentWithRetryable) {
    for (F16aErrorCode code : AllCodes()) {
        const F16aErrorInfo& info = GetF16aErrorInfo(code);
        EXPECT_EQ(info.retryable, info.retry_after_ms.has_value())
            << F16aErrorCodeString(code);
    }
}

// MakeF16aError fills the 4 GEN-Agent fields from the registry (call sites never
// restate category/retryable/retry_after_ms).
TEST(F16aErrorTest, MakeErrorPopulatesAgentFriendlyFields) {
    auto err = MakeF16aError(F16aErrorCode::kTimeout,
                             {{"task_id", "import_x"},
                              {"timeout_seconds", 300},
                              {"rows_imported_before_timeout", 4500}},
                             "Query exceeded 300s timeout on table users");
    EXPECT_EQ(err.code, "CX_ERR_F16A_TIMEOUT");
    EXPECT_TRUE(err.retryable);
    EXPECT_EQ(err.category, ErrorCategory::kTimeout);
    ASSERT_TRUE(err.retry_after_ms.has_value());
    EXPECT_EQ(*err.retry_after_ms, 30000);
    ASSERT_TRUE(err.structured_data.has_value());
    EXPECT_EQ((*err.structured_data)["timeout_seconds"], 300);
}

// Empty message falls back to the CX_ERR_ token (never an empty string).
TEST(F16aErrorTest, EmptyMessageFallsBackToCode) {
    auto err = MakeF16aError(F16aErrorCode::kAuthDenied);
    EXPECT_EQ(err.message, "CX_ERR_F16A_AUTH_DENIED");
}

// Required structured_data keys (GEN-Agent #5) for the codes that declare them.
TEST(F16aErrorTest, RequiredStructuredDataContract) {
    EXPECT_TRUE(RequiredStructuredDataKeys(F16aErrorCode::kConnectionFailed).empty());
    EXPECT_TRUE(RequiredStructuredDataKeys(F16aErrorCode::kAuthDenied).empty());
    EXPECT_TRUE(RequiredStructuredDataKeys(F16aErrorCode::kInvalidSql).empty());

    nlohmann::json full = {{"task_id", "t"}, {"timeout_seconds", 300},
                           {"rows_imported_before_timeout", 1}};
    EXPECT_TRUE(HasRequiredStructuredData(F16aErrorCode::kTimeout, full));
    nlohmann::json partial = {{"task_id", "t"}};
    EXPECT_FALSE(HasRequiredStructuredData(F16aErrorCode::kTimeout, partial));

    nlohmann::json rows = {{"max_rows", 10000000}, {"estimated_rows", 12345678}};
    EXPECT_TRUE(HasRequiredStructuredData(F16aErrorCode::kRowsLimitExceeded, rows));

    nlohmann::json xt = {{"connection_ref", "db_conn_a"}, {"tenant_id", "t1"}};
    EXPECT_TRUE(HasRequiredStructuredData(F16aErrorCode::kCrossTenantRef, xt));
}

// F16aStatus bridges to a coarse Status whose message recovers the exact identity
// (F-FREEZE-1: no Result<T,E>; identity travels in the CX_ERR_ prefix).
TEST(F16aErrorTest, StatusBridgePreservesIdentityInMessage) {
    Status s = F16aStatus(F16aErrorCode::kCrossTenantRef, "ref db_conn_a not in tenant t1");
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kPermissionDenied);
    EXPECT_NE(s.message().find("CX_ERR_F16A_CROSS_TENANT_REF"), std::string::npos);

    EXPECT_EQ(F16aStatus(F16aErrorCode::kInvalidSql).code(), StatusCode::kInvalidArgument);
    EXPECT_EQ(F16aStatus(F16aErrorCode::kConnectionFailed).code(), StatusCode::kUnavailable);
    EXPECT_EQ(F16aStatus(F16aErrorCode::kTimeout).code(), StatusCode::kUnavailable);
    EXPECT_EQ(F16aStatus(F16aErrorCode::kRowsLimitExceeded).code(), StatusCode::kInvalidArgument);
}

// F16aException carries the full Agent-friendly error for the throw-to-handler path.
TEST(F16aErrorTest, ExceptionCarriesFullError) {
    try {
        throw F16aException(F16aErrorCode::kInvalidSql, nlohmann::json::object(),
                            "denied keyword UNION");
    } catch (const F16aException& e) {
        EXPECT_EQ(e.code(), F16aErrorCode::kInvalidSql);
        EXPECT_EQ(e.GetError().code, "CX_ERR_F16A_INVALID_SQL");
        EXPECT_EQ(e.GetError().message, "denied keyword UNION");
    }
}

// --- §5.1 / §5.2 / §5.3 Agent-friendly response bodies ---

TEST(F16aResponseTest, ImportStartedBodyMatchesSpec) {
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

TEST(F16aResponseTest, ProgressBodyRunningHasNullError) {
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

TEST(F16aResponseTest, ProgressBodyFailedEmbedsErrorBody) {
    ImportTaskProgress task;
    task.task_id = "import_xyz";
    task.status = ImportTaskStatus::kFailed;
    task.error = MakeF16aError(F16aErrorCode::kTimeout,
                               {{"task_id", "import_xyz"},
                                {"timeout_seconds", 300},
                                {"rows_imported_before_timeout", 4500}},
                               "Query exceeded 300s timeout on table users");

    auto body = BuildProgressResponse(task);
    EXPECT_EQ(body["status"], "failed");
    ASSERT_TRUE(body["error"].is_object());
    EXPECT_EQ(body["error"]["code"], "CX_ERR_F16A_TIMEOUT");
    EXPECT_EQ(body["error"]["retryable"], true);
    EXPECT_EQ(body["error"]["category"], "timeout");
    EXPECT_EQ(body["error"]["retry_after_ms"], 30000);
}

// Cancelled is a clean terminal state, NOT an error (§5.4 note).
TEST(F16aResponseTest, CancelledHasNoErrorBody) {
    ImportTaskProgress task;
    task.task_id = "import_xyz";
    task.status = ImportTaskStatus::kCancelled;
    auto body = BuildProgressResponse(task);
    EXPECT_EQ(body["status"], "cancelled");
    EXPECT_TRUE(body["error"].is_null());
}

// text_strategy wire parsing: per_row/merge accepted, template rejected (V1.0).
TEST(F16aTypesTest, ParseTextStrategyRejectsTemplate) {
    EXPECT_EQ(ParseTextStrategy("per_row"), TextStrategy::kPerRow);
    EXPECT_EQ(ParseTextStrategy("merge"), TextStrategy::kMerge);
    EXPECT_FALSE(ParseTextStrategy("template").has_value());
    EXPECT_FALSE(ParseTextStrategy("").has_value());
    EXPECT_STREQ(ToString(TextStrategy::kPerRow), "per_row");
    EXPECT_STREQ(ToString(TextStrategy::kMerge), "merge");
}

}  // namespace
}  // namespace cortrix::import
