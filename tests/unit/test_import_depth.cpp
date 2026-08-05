// DEPTH anti-regression tests for src/import/ -- targets genuinely untested public
// surface: import_types ToString/ParseTextStrategy (no dedicated test), import_response
// Build*/ToIso8601Utc (no dedicated test), and edge/race cases of InMemoryImportTaskStore
// + ImportTaskQueue not covered by test_import_task_queue.cpp.
//
// Deterministic: in-memory store only, no PG/network/LLM. Worker threads come from the
// shared ExecutorEngine; tests use a spin-wait helper with a bounded deadline so they
// cannot hang.
//
// Suite/fixture names are globally unique (ImportDepth* prefix) per gtest's global-string
// suite-name rule.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iterator>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "cortrix/agent_friendly/error.h"
#include "cortrix/import/import_error.h"
#include "cortrix/import/import_response.h"
#include "cortrix/import/import_task_queue.h"
#include "cortrix/import/import_types.h"

namespace cortrix::import {
namespace {

template <typename Pred>
bool ImportDepthWaitFor(Pred pred, int timeout_ms = 2000) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return pred();
}

// --------------------------------------------------------------------------
// import_types: TextStrategy + ImportTaskStatus ToString / ParseTextStrategy
// --------------------------------------------------------------------------

TEST(ImportDepthTypesTest, TextStrategyToStringRoundTrip) {
    EXPECT_STREQ(ToString(TextStrategy::kPerRow), "per_row");
    EXPECT_STREQ(ToString(TextStrategy::kMerge), "merge");
}

TEST(ImportDepthTypesTest, ParseTextStrategyAcceptsKnownTokens) {
    auto per_row = ParseTextStrategy("per_row");
    ASSERT_TRUE(per_row.has_value());
    EXPECT_EQ(*per_row, TextStrategy::kPerRow);
    auto merge = ParseTextStrategy("merge");
    ASSERT_TRUE(merge.has_value());
    EXPECT_EQ(*merge, TextStrategy::kMerge);
}

TEST(ImportDepthTypesTest, ParseTextStrategyRejectsTemplateAndUnknown) {
    // "template" was deliberately removed in V1.0 (D1 V3 resolution 13).
    EXPECT_FALSE(ParseTextStrategy("template").has_value());
    EXPECT_FALSE(ParseTextStrategy("PER_ROW").has_value());  // case sensitive
    EXPECT_FALSE(ParseTextStrategy("").has_value());
    EXPECT_FALSE(ParseTextStrategy("jinja").has_value());
}

TEST(ImportDepthTypesTest, ParseTextStrategyRoundTripsToString) {
    for (auto s : {TextStrategy::kPerRow, TextStrategy::kMerge}) {
        auto parsed = ParseTextStrategy(ToString(s));
        ASSERT_TRUE(parsed.has_value());
        EXPECT_EQ(*parsed, s);
    }
}

TEST(ImportDepthTypesTest, ImportTaskStatusToStringAllStates) {
    EXPECT_STREQ(ToString(ImportTaskStatus::kQueued), "queued");
    EXPECT_STREQ(ToString(ImportTaskStatus::kRunning), "running");
    EXPECT_STREQ(ToString(ImportTaskStatus::kCompleted), "completed");
    EXPECT_STREQ(ToString(ImportTaskStatus::kFailed), "failed");
    EXPECT_STREQ(ToString(ImportTaskStatus::kCancelling), "cancelling");
    EXPECT_STREQ(ToString(ImportTaskStatus::kCancelled), "cancelled");
}

// --------------------------------------------------------------------------
// import_response: ToIso8601Utc + BuildImportStartedResponse + BuildProgressResponse
// --------------------------------------------------------------------------

TEST(ImportDepthResponseTest, Iso8601UtcEpochIsZulu) {
    // Unix epoch 0 -> 1970-01-01T00:00:00Z (UTC, seconds granularity, trailing Z).
    auto epoch = std::chrono::system_clock::time_point{};
    EXPECT_EQ(ToIso8601Utc(epoch), "1970-01-01T00:00:00Z");
}

TEST(ImportDepthResponseTest, Iso8601UtcKnownInstant) {
    using namespace std::chrono;
    // 2021-01-01T00:00:00Z = 1609459200 seconds since epoch.
    system_clock::time_point tp{seconds{1609459200}};
    EXPECT_EQ(ToIso8601Utc(tp), "2021-01-01T00:00:00Z");
}

TEST(ImportDepthResponseTest, ImportStartedBodyForcesQueuedStatusAndNullError) {
    ImportTaskProgress task;
    task.task_id = "import_abc";
    task.namespace_id = "ns1";
    // Even if a worker already advanced status, the ACCEPTANCE ack must echo "queued".
    task.status = ImportTaskStatus::kRunning;
    task.queued_at = std::chrono::system_clock::time_point{};
    auto body = BuildImportStartedResponse(task, "db_conn_xyz", 4242);
    EXPECT_EQ(body["task_id"], "import_abc");
    EXPECT_EQ(body["status"], "queued");
    EXPECT_EQ(body["namespace"], "ns1");
    EXPECT_EQ(body["connection_ref"], "db_conn_xyz");
    EXPECT_EQ(body["estimated_rows"], 4242);
    EXPECT_EQ(body["queued_at"], "1970-01-01T00:00:00Z");
    EXPECT_EQ(body["progress_endpoint"], "/api/v1/import/tasks/import_abc/progress");
    EXPECT_EQ(body["cancel_endpoint"], "/api/v1/import/tasks/import_abc");
    EXPECT_TRUE(body["error"].is_null());
}

TEST(ImportDepthResponseTest, ProgressBodyNonTerminalHasNullErrorAndNullTimestamps) {
    ImportTaskProgress task;
    task.task_id = "import_run";
    task.status = ImportTaskStatus::kRunning;
    task.progress = 0.5;
    task.rows_imported = 5;
    task.rows_total = 10;
    task.rows_failed = 1;
    // started_at / estimated_completion_at left as nullopt -> JSON null.
    auto body = BuildProgressResponse(task);
    EXPECT_EQ(body["status"], "running");
    EXPECT_DOUBLE_EQ(body["progress"].get<double>(), 0.5);
    EXPECT_EQ(body["rows_imported"], 5);
    EXPECT_EQ(body["rows_total"], 10);
    EXPECT_EQ(body["rows_failed"], 1);
    EXPECT_TRUE(body["started_at"].is_null());
    EXPECT_TRUE(body["estimated_completion_at"].is_null());
    EXPECT_TRUE(body["error"].is_null());
}

TEST(ImportDepthResponseTest, ProgressBodyFailedEmbedsErrorBody) {
    ImportTaskProgress task;
    task.task_id = "import_fail";
    task.status = ImportTaskStatus::kFailed;
    task.error = MakeF16aError(F16aErrorCode::kConnectionFailed, nlohmann::json::object(),
                               "pg down");
    auto body = BuildProgressResponse(task);
    EXPECT_EQ(body["status"], "failed");
    ASSERT_TRUE(body["error"].is_object());
    EXPECT_EQ(body["error"]["code"], F16aErrorCodeString(F16aErrorCode::kConnectionFailed));
}

TEST(ImportDepthResponseTest, ProgressBodyCancelledHasNullErrorPerSpecNote) {
    // section 5.4 note: cancel is not an error -- cancelled state carries no error body.
    ImportTaskProgress task;
    task.task_id = "import_cancel";
    task.status = ImportTaskStatus::kCancelled;
    auto body = BuildProgressResponse(task);
    EXPECT_EQ(body["status"], "cancelled");
    EXPECT_TRUE(body["error"].is_null());
}

TEST(ImportDepthResponseTest, ProgressBodyTimestampsSerializeWhenPresent) {
    ImportTaskProgress task;
    task.task_id = "import_ts";
    task.status = ImportTaskStatus::kRunning;
    task.started_at = std::chrono::system_clock::time_point{};
    task.estimated_completion_at =
        std::chrono::system_clock::time_point{std::chrono::seconds{1609459200}};
    auto body = BuildProgressResponse(task);
    EXPECT_EQ(body["started_at"], "1970-01-01T00:00:00Z");
    EXPECT_EQ(body["estimated_completion_at"], "2021-01-01T00:00:00Z");
}

// --------------------------------------------------------------------------
// InMemoryImportTaskStore -- CRUD + CAS edges not covered elsewhere
// --------------------------------------------------------------------------

class ImportDepthStoreFx : public ::testing::Test {
protected:
    static ImportTaskProgress MakeTask(const std::string& id, const std::string& ns,
                                       ImportTaskStatus st = ImportTaskStatus::kQueued) {
        ImportTaskProgress t;
        t.task_id = id;
        t.namespace_id = ns;
        t.status = st;
        return t;
    }
    InMemoryImportTaskStore store_;
};

TEST_F(ImportDepthStoreFx, GetUnknownReturnsNullopt) {
    EXPECT_FALSE(store_.Get("nope").has_value());
}

TEST_F(ImportDepthStoreFx, UpdateUnknownReturnsNotFound) {
    auto st = store_.Update(MakeTask("ghost", "ns"));
    EXPECT_EQ(st.code(), StatusCode::kNotFound);
}

TEST_F(ImportDepthStoreFx, UpdateOverwritesMutableFields) {
    ASSERT_TRUE(store_.Create(MakeTask("t1", "ns")).ok());
    auto t = MakeTask("t1", "ns", ImportTaskStatus::kRunning);
    t.rows_imported = 7;
    t.progress = 0.7;
    ASSERT_TRUE(store_.Update(t).ok());
    auto got = store_.Get("t1");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->status, ImportTaskStatus::kRunning);
    EXPECT_EQ(got->rows_imported, 7);
    EXPECT_DOUBLE_EQ(got->progress, 0.7);
}

TEST_F(ImportDepthStoreFx, CompareAndSetUnknownTaskIsFalse) {
    EXPECT_FALSE(store_.CompareAndSetStatus("missing", ImportTaskStatus::kQueued,
                                            ImportTaskStatus::kRunning));
}

TEST_F(ImportDepthStoreFx, CompareAndSetWrongExpectedDoesNotWrite) {
    ASSERT_TRUE(store_.Create(MakeTask("t2", "ns", ImportTaskStatus::kQueued)).ok());
    // expected=kRunning but actual is kQueued -> no transition.
    EXPECT_FALSE(store_.CompareAndSetStatus("t2", ImportTaskStatus::kRunning,
                                            ImportTaskStatus::kCompleted));
    EXPECT_EQ(store_.Get("t2")->status, ImportTaskStatus::kQueued);
}

TEST_F(ImportDepthStoreFx, CompareAndSetSucceedsOnlyOnceForSameTransition) {
    ASSERT_TRUE(store_.Create(MakeTask("t3", "ns", ImportTaskStatus::kQueued)).ok());
    EXPECT_TRUE(store_.CompareAndSetStatus("t3", ImportTaskStatus::kQueued,
                                           ImportTaskStatus::kRunning));
    // A second attempt with the same expected now fails (status already advanced).
    EXPECT_FALSE(store_.CompareAndSetStatus("t3", ImportTaskStatus::kQueued,
                                            ImportTaskStatus::kRunning));
}

TEST_F(ImportDepthStoreFx, ListByNamespaceScopesAndExcludesOthers) {
    ASSERT_TRUE(store_.Create(MakeTask("a", "nsA")).ok());
    ASSERT_TRUE(store_.Create(MakeTask("b", "nsA")).ok());
    ASSERT_TRUE(store_.Create(MakeTask("c", "nsB")).ok());
    auto a = store_.ListByNamespace("nsA");
    EXPECT_EQ(a.size(), 2u);
    auto b = store_.ListByNamespace("nsB");
    EXPECT_EQ(b.size(), 1u);
    EXPECT_TRUE(store_.ListByNamespace("nsZ").empty());
}

TEST_F(ImportDepthStoreFx, ReportProgressViaHandleComputesRatio) {
    ASSERT_TRUE(store_.Create(MakeTask("h1", "ns")).ok());
    auto flag = std::make_shared<std::atomic<bool>>(false);
    ImportTaskHandle handle("h1", &store_, flag);
    EXPECT_FALSE(handle.CancelRequested());
    handle.ReportProgress(3, 12);
    auto got = store_.Get("h1");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->rows_imported, 3);
    EXPECT_EQ(got->rows_total, 12);
    EXPECT_DOUBLE_EQ(got->progress, 0.25);
}

TEST_F(ImportDepthStoreFx, ReportProgressZeroTotalKeepsRatioZero) {
    ASSERT_TRUE(store_.Create(MakeTask("h2", "ns")).ok());
    auto flag = std::make_shared<std::atomic<bool>>(false);
    ImportTaskHandle handle("h2", &store_, flag);
    handle.ReportProgress(5, 0);  // rows_total unknown -> ratio stays 0
    auto got = store_.Get("h2");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->rows_imported, 5);
    EXPECT_DOUBLE_EQ(got->progress, 0.0);
}

TEST_F(ImportDepthStoreFx, HandleCancelRequestedReflectsFlag) {
    ASSERT_TRUE(store_.Create(MakeTask("h3", "ns")).ok());
    auto flag = std::make_shared<std::atomic<bool>>(false);
    ImportTaskHandle handle("h3", &store_, flag);
    flag->store(true);
    EXPECT_TRUE(handle.CancelRequested());
}

// --------------------------------------------------------------------------
// ImportTaskQueue -- boundary + queue-full + double-cancel edges
// --------------------------------------------------------------------------

TEST(ImportDepthQueueTest, GetProgressUnknownTaskReturnsNullopt) {
    auto store = std::make_shared<InMemoryImportTaskStore>();
    ImportTaskQueue queue(store);
    EXPECT_FALSE(queue.GetProgress("unknown").has_value());
}

TEST(ImportDepthQueueTest, QueueFullSurfacedAsTransientConnectionFailed) {
    auto store = std::make_shared<InMemoryImportTaskStore>();
    ImportTaskQueueConfig cfg;
    cfg.worker_count = 1;
    cfg.queue_max_size = 1;  // only one in-flight slot
    ImportTaskQueue queue(store, cfg);

    auto gate = std::make_shared<std::atomic<bool>>(false);
    // First task blocks the single worker until released.
    auto blocking_work = [gate](ImportTaskHandle&) -> Status {
        while (!gate->load()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        return Status::Ok();
    };
    auto r1 = queue.Submit("t1", "ns", blocking_work);
    ASSERT_TRUE(r1.ok());

    // Second submit exceeds queue_max_size while t1 still occupies the slot.
    auto r2 = queue.Submit("t2", "ns", [](ImportTaskHandle&) { return Status::Ok(); });
    ASSERT_FALSE(r2.ok());
    EXPECT_NE(r2.status().message().find("CX_ERR_IMPORT_"), std::string::npos);

    gate->store(true);  // release the blocking worker
    queue.DrainForTest();
}

TEST(ImportDepthQueueTest, CancelTerminalCompletedIsNoOpButReturnsTrue) {
    auto store = std::make_shared<InMemoryImportTaskStore>();
    ImportTaskQueue queue(store);
    auto r = queue.Submit("done", "ns", [](ImportTaskHandle&) { return Status::Ok(); });
    ASSERT_TRUE(r.ok());
    ASSERT_TRUE(ImportDepthWaitFor([&] {
        auto p = queue.GetProgress("done");
        return p && p->status == ImportTaskStatus::kCompleted;
    }));
    // Cancel on a terminal task: known task -> returns true, status unchanged.
    EXPECT_TRUE(queue.Cancel("done"));
    EXPECT_EQ(queue.GetProgress("done")->status, ImportTaskStatus::kCompleted);
}

TEST(ImportDepthQueueTest, CompletedTaskWithoutTotalBackfillsFromImported) {
    auto store = std::make_shared<InMemoryImportTaskStore>();
    ImportTaskQueue queue(store);
    // Worker reports rows but submit gave rows_total=0 (unknown).
    auto work = [](ImportTaskHandle& h) -> Status {
        h.ReportProgress(9, 0);
        return Status::Ok();
    };
    auto r = queue.Submit("bf", "ns", work, /*rows_total=*/0);
    ASSERT_TRUE(r.ok());
    ASSERT_TRUE(ImportDepthWaitFor([&] {
        auto p = queue.GetProgress("bf");
        return p && p->status == ImportTaskStatus::kCompleted;
    }));
    auto p = queue.GetProgress("bf");
    ASSERT_TRUE(p.has_value());
    EXPECT_DOUBLE_EQ(p->progress, 1.0);
    EXPECT_EQ(p->rows_total, 9);  // backfilled from rows_imported
}

TEST(ImportDepthQueueTest, FailedWorkWithoutCxPrefixKeepsDefaultConnectionFailedCode) {
    auto store = std::make_shared<InMemoryImportTaskStore>();
    ImportTaskQueue queue(store);
    // Work returns a plain Status whose message lacks the CX_ERR_IMPORT_ prefix -> the
    // queue keeps the default kConnectionFailed identity for the error body.
    auto work = [](ImportTaskHandle&) -> Status {
        return Status::Internal("raw driver blew up");
    };
    auto r = queue.Submit("ferr", "ns", work);
    ASSERT_TRUE(r.ok());
    ASSERT_TRUE(ImportDepthWaitFor([&] {
        auto p = queue.GetProgress("ferr");
        return p && p->status == ImportTaskStatus::kFailed;
    }));
    auto p = queue.GetProgress("ferr");
    ASSERT_TRUE(p.has_value());
    ASSERT_TRUE(p->error.has_value());
    EXPECT_EQ(p->error->code, F16aErrorCodeString(F16aErrorCode::kConnectionFailed));
}

TEST(ImportDepthQueueTest, FailedWorkWithCxPrefixRecoversPreciseCode) {
    auto store = std::make_shared<InMemoryImportTaskStore>();
    ImportTaskQueue queue(store);
    // Work returns an F16aStatus whose message is "CX_ERR_IMPORT_TIMEOUT: ...".
    auto work = [](ImportTaskHandle&) -> Status {
        return F16aStatus(F16aErrorCode::kTimeout, "exceeded 5 min");
    };
    auto r = queue.Submit("ferr2", "ns", work);
    ASSERT_TRUE(r.ok());
    ASSERT_TRUE(ImportDepthWaitFor([&] {
        auto p = queue.GetProgress("ferr2");
        return p && p->status == ImportTaskStatus::kFailed;
    }));
    auto p = queue.GetProgress("ferr2");
    ASSERT_TRUE(p.has_value());
    ASSERT_TRUE(p->error.has_value());
    EXPECT_EQ(p->error->code, F16aErrorCodeString(F16aErrorCode::kTimeout));
}

// --------------------------------------------------------------------------
// import_error: registry-driven error-body completeness (DB import section 5.4)
// --------------------------------------------------------------------------

TEST(ImportDepthErrorTest, MakeErrorFillsCategoryRetryableFromRegistry) {
    auto err = MakeF16aError(F16aErrorCode::kConnectionFailed);
    const auto& info = GetF16aErrorInfo(F16aErrorCode::kConnectionFailed);
    EXPECT_EQ(err.code, info.cx_code);
    EXPECT_EQ(err.retryable, info.retryable);
    EXPECT_EQ(err.category, info.category);
}

TEST(ImportDepthErrorTest, MakeErrorDefaultMessageFallsBackToCode) {
    auto err = MakeF16aError(F16aErrorCode::kInvalidSql);  // no explicit message
    EXPECT_FALSE(err.message.empty());
}

TEST(ImportDepthErrorTest, AllErrorCodeStringsAreUniqueAndCountMatches) {
    const F16aErrorCode all[] = {
        F16aErrorCode::kConnectionFailed, F16aErrorCode::kAuthDenied,
        F16aErrorCode::kInvalidSql,       F16aErrorCode::kTimeout,
        F16aErrorCode::kRowsLimitExceeded, F16aErrorCode::kCrossTenantRef,
        F16aErrorCode::kInternal,
    };
    EXPECT_EQ(static_cast<int>(std::size(all)), kF16aErrorCodeCount);
    std::vector<std::string> codes;
    for (auto c : all) codes.emplace_back(F16aErrorCodeString(c));
    std::sort(codes.begin(), codes.end());
    EXPECT_EQ(std::adjacent_find(codes.begin(), codes.end()), codes.end());
}

TEST(ImportDepthErrorTest, StatusBridgeCarriesCxToken) {
    auto st = F16aStatus(F16aErrorCode::kTimeout, "5 min cap");
    EXPECT_EQ(st.message().rfind("CX_ERR_IMPORT_", 0), 0u);
    EXPECT_NE(st.message().find("5 min cap"), std::string::npos);
}

TEST(ImportDepthErrorTest, ToStatusCodeMapsTimeoutAndAuthDistinctly) {
    // Timeout is transient (kUnavailable family) while auth is permission-denied -- the
    // coarse mapping must not collapse them to the same code.
    auto timeout = F16aErrorToStatusCode(F16aErrorCode::kTimeout);
    auto auth = F16aErrorToStatusCode(F16aErrorCode::kAuthDenied);
    EXPECT_NE(timeout, auth);
    EXPECT_EQ(auth, StatusCode::kPermissionDenied);
}

// --------------------------------------------------------------------------
// ImportRequest defaults + endpoint formatting edges
// --------------------------------------------------------------------------

TEST(ImportDepthTypesTest, ImportRequestDefaultsArePerRowAndMergeSize10) {
    ImportRequest req;
    EXPECT_EQ(req.text_strategy, TextStrategy::kPerRow);
    EXPECT_EQ(req.merge_size, 10);
}

TEST(ImportDepthResponseTest, EndpointsEmbedTaskIdVerbatim) {
    ImportTaskProgress task;
    task.task_id = "import_01HX";  // ULID-style id
    task.namespace_id = "ns";
    task.queued_at = std::chrono::system_clock::time_point{};
    auto body = BuildImportStartedResponse(task, "ref", 0);
    EXPECT_EQ(body["progress_endpoint"], "/api/v1/import/tasks/import_01HX/progress");
    EXPECT_EQ(body["cancel_endpoint"], "/api/v1/import/tasks/import_01HX");
}

TEST(ImportDepthResponseTest, ImportStartedZeroEstimatedRows) {
    ImportTaskProgress task;
    task.task_id = "t";
    task.namespace_id = "ns";
    task.queued_at = std::chrono::system_clock::time_point{};
    auto body = BuildImportStartedResponse(task, "ref", /*estimated_rows=*/0);
    EXPECT_EQ(body["estimated_rows"], 0);
}

// --------------------------------------------------------------------------
// ImportTaskHandle on a missing task: ReportProgress is a safe no-op
// --------------------------------------------------------------------------

TEST(ImportDepthQueueTest, ReportProgressOnMissingTaskIsNoOp) {
    InMemoryImportTaskStore store;
    auto flag = std::make_shared<std::atomic<bool>>(false);
    ImportTaskHandle handle("never-created", &store, flag);
    handle.ReportProgress(5, 10);  // Get() returns nullopt -> no write, no crash
    EXPECT_FALSE(store.Get("never-created").has_value());
}

TEST(ImportDepthQueueTest, CancelWhileQueuedNeverRunsWorkAndIsCancelled) {
    auto store = std::make_shared<InMemoryImportTaskStore>();
    ImportTaskQueueConfig cfg;
    cfg.worker_count = 1;
    ImportTaskQueue queue(store, cfg);

    // Hold the single worker on a different task so our target stays QUEUED.
    auto gate = std::make_shared<std::atomic<bool>>(false);
    ASSERT_TRUE(queue.Submit("blocker", "ns",
                             [gate](ImportTaskHandle&) -> Status {
                                 while (!gate->load())
                                     std::this_thread::sleep_for(std::chrono::milliseconds(2));
                                 return Status::Ok();
                             })
                    .ok());

    std::atomic<bool> ran{false};
    ASSERT_TRUE(queue.Submit("target", "ns",
                             [&ran](ImportTaskHandle&) -> Status {
                                 ran = true;
                                 return Status::Ok();
                             })
                    .ok());
    // Cancel while still queued -> CANCELLED directly, work never runs.
    EXPECT_TRUE(queue.Cancel("target"));
    gate->store(true);
    queue.DrainForTest();

    auto p = queue.GetProgress("target");
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(p->status, ImportTaskStatus::kCancelled);
    EXPECT_FALSE(ran.load());
}

}  // namespace
}  // namespace cortrix::import
