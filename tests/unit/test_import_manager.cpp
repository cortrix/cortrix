#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

#include "cortrix/import/connection_manager.h"
#include "cortrix/import/import_error.h"
#include "cortrix/import/import_manager.h"
#include "cortrix/import/import_metrics.h"
#include "cortrix/import/query_executor.h"
#include "cortrix/import/spc_feed.h"
#include "cortrix/import/text_serializer.h"
#include "cortrix/observability/operation_logger.h"
#include "cortrix/server/import_handler.h"

// S3 + S4 + S6 coverage: ImportManager end-to-end orchestration (ref resolve →
// fetch → textualize → cleanup → feed → async), the operation log
// operation_loginstrumentation, the metrics, and the HTTP handler parse/response. Real PG /
// SPCPipeline / server routing are mocked → integration.
namespace cortrix::import {
namespace {

// --- a fake IQueryExecutor that returns canned rows (real PG → integration) ---
class FakeQueryExecutor : public IQueryExecutor {
public:
    explicit FakeQueryExecutor(std::vector<DbRow> rows) : rows_(std::move(rows)) {}
    Result<int64_t> EstimateRowCount(const std::string&, const QueryRequest&,
                                     const QueryConstraints&) override {
        return static_cast<int64_t>(rows_.size());
    }
    Result<std::vector<DbRow>> Execute(const std::string&, const QueryRequest&,
                                       const QueryConstraints&) override {
        return rows_;
    }
private:
    std::vector<DbRow> rows_;
};

// --- a fake operation logger that records entries (real CE logger is concrete) ---
class FakeOpLogger : public observability::IOperationLogger {
public:
    void Log(const observability::OperationLogEntry& e,
             const observability::TraceContext*) override {
        std::lock_guard<std::mutex> lk(mu_);
        entries_.push_back(e);
    }
    void BatchLog(const std::vector<observability::OperationLogEntry>& es,
                  const observability::TraceContext*) override {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& e : es) entries_.push_back(e);
    }
    Result<observability::OperationLogQueryResult> Query(
        const observability::OperationLogFilter&,
        const observability::TraceContext*) override {
        return observability::OperationLogQueryResult{};
    }
    void Cleanup() override {}
    observability::OperationLogStats GetStats() override { return {}; }
    observability::HealthStatus Health() override { return {}; }

    std::vector<observability::OperationLogEntry> entries() {
        std::lock_guard<std::mutex> lk(mu_);
        return entries_;
    }
private:
    std::mutex mu_;
    std::vector<observability::OperationLogEntry> entries_;
};

DbRow Row(const std::string& id, const std::string& name) {
    DbRow r;
    r.row_id = id;
    r.columns = {{"id", DbValue{id, false}}, {"name", DbValue{name, false}}};
    return r;
}

AuthContext AdminCtx(const std::string& tenant = "t1", const std::string& user = "admin1") {
    AuthContext c;
    c.tenant_id = tenant;
    c.user_id = user;
    c.permissions = kPermAdmin | kPermRead | kPermWrite;
    return c;
}

template <typename Pred>
bool WaitFor(Pred pred, int timeout_ms = 2000) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return pred();
}

struct ManagerFixture {
    std::shared_ptr<ConnectionManager> conn_mgr;
    std::shared_ptr<FakeQueryExecutor> query_exec;
    std::shared_ptr<TextSerializer> serializer;
    std::shared_ptr<InMemorySpcFeeder> feeder;
    std::shared_ptr<InMemoryBlockCleaner> cleaner;
    std::shared_ptr<InMemoryImportTaskStore> task_store;
    std::shared_ptr<FakeOpLogger> oplog;
    std::shared_ptr<ImportManager> mgr;
    ConnectionRefId ref;

    ManagerFixture(std::vector<DbRow> rows) {
        auto secret = std::make_shared<InMemorySecretStore>();
        auto conn_store = std::make_shared<InMemoryConnectionStore>();
        conn_mgr = std::make_shared<ConnectionManager>(secret, conn_store);
        auto reg = conn_mgr->Register("crm", "postgres://u:p@db.internal:5432/crm", "t1", "admin1");
        ref = reg.value();

        query_exec = std::make_shared<FakeQueryExecutor>(std::move(rows));
        serializer = std::make_shared<TextSerializer>();
        feeder = std::make_shared<InMemorySpcFeeder>();
        cleaner = std::make_shared<InMemoryBlockCleaner>();
        task_store = std::make_shared<InMemoryImportTaskStore>();
        oplog = std::make_shared<FakeOpLogger>();

        ImportManagerConfig cfg;
        cfg.pg_host = "db.internal";
        cfg.pg_port = 5432;
        cfg.pg_db = "crm";
        mgr = std::make_shared<ImportManager>(conn_mgr, query_exec, serializer, feeder, cleaner,
                                              task_store, oplog, cfg);
    }

    ImportRequest Req(const std::string& table = "users") {
        ImportRequest r;
        r.namespace_id = "customer_kb";
        r.connection_ref = ref;
        r.query.table = table;
        return r;
    }
};

TEST(ImportManagerTest, EndToEndImportFeedsChunksAndCompletes) {
    ImportMetrics::Instance().ResetForTest();
    ManagerFixture f({Row("1", "Ada"), Row("2", "Bob")});

    auto r = f.mgr->StartImport(f.Req(), AdminCtx());
    ASSERT_TRUE(r.ok()) << r.status().message();
    ImportTaskId task_id = r.value();

    EXPECT_TRUE(WaitFor([&] {
        auto p = f.mgr->GetProgress(task_id);
        return p && p->status == ImportTaskStatus::kCompleted;
    }));

    //: the 2 rows were textualized + fed to the SPC pipeline for the right ns.
    EXPECT_EQ(f.feeder->total_chunks(), 2);
    ASSERT_EQ(f.feeder->batches().size(), 1u);
    EXPECT_EQ(f.feeder->batches()[0].ns, "customer_kb");
    // source URI stamped on the chunk.
    EXPECT_EQ(f.feeder->batches()[0].chunks[0].source,
              "postgres://db.internal:5432/crm/users/1");

    // S6: operation_log `database_import` written with the right resource_type + user.
    auto entries = f.oplog->entries();
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].action, "database_import");
    EXPECT_EQ(entries[0].resource_type, "db_import");
    EXPECT_EQ(entries[0].user_id, "admin1");
    ASSERT_TRUE(entries[0].namespace_id.has_value());
    EXPECT_EQ(*entries[0].namespace_id, "customer_kb");

    // metrics: 1 success + 2 rows.
    EXPECT_EQ(ImportMetrics::Instance().ImportsCount(ImportMetrics::ImportOutcome::kSuccess), 1u);
    EXPECT_EQ(ImportMetrics::Instance().RowsImportedTotal(), 2u);
}

TEST(ImportManagerTest, NonAdminRejectedSynchronously) {
    ManagerFixture f({Row("1", "Ada")});
    AuthContext non_admin;
    non_admin.tenant_id = "t1";
    non_admin.user_id = "u";
    non_admin.permissions = kPermRead | kPermWrite;  // no admin
    auto r = f.mgr->StartImport(f.Req(), non_admin);
    EXPECT_FALSE(r.ok());
    EXPECT_NE(r.status().message().find("CX_ERR_IMPORT_AUTH_DENIED"), std::string::npos);
}

TEST(ImportManagerTest, CrossTenantRefThrows) {
    ManagerFixture f({Row("1", "Ada")});
    // admin of a DIFFERENT tenant tries to use t1's ref.
    EXPECT_THROW((void)f.mgr->StartImport(f.Req(), AdminCtx("t2", "evil")), ImportException);
}

TEST(ImportManagerTest, InvalidQueryRejectedSynchronously) {
    ManagerFixture f({Row("1", "Ada")});
    ImportRequest req = f.Req();
    req.query.table = std::nullopt;
    req.query.sql = "DROP TABLE users";  // write verb
    auto r = f.mgr->StartImport(req, AdminCtx());
    EXPECT_FALSE(r.ok());
    EXPECT_NE(r.status().message().find("CX_ERR_IMPORT_INVALID_SQL"), std::string::npos);
}

TEST(ImportManagerTest, FullOverwriteClearsPriorTableBlocks) {
    ManagerFixture f({Row("1", "Ada")});
    // seed prior Blocks: 2 from this table, 1 from another table.
    f.cleaner->SeedBlock("customer_kb", "postgres://db.internal:5432/crm/users/old1");
    f.cleaner->SeedBlock("customer_kb", "postgres://db.internal:5432/crm/users/old2");
    f.cleaner->SeedBlock("customer_kb", "postgres://db.internal:5432/crm/orders/x");

    auto r = f.mgr->StartImport(f.Req("users"), AdminCtx());
    ASSERT_TRUE(r.ok());
    EXPECT_TRUE(WaitFor([&] {
        auto p = f.mgr->GetProgress(r.value());
        return p && p->status == ImportTaskStatus::kCompleted;
    }));
    // only the orders Block survives (the 2 prior users Blocks were cleared by).
    EXPECT_EQ(f.cleaner->RemainingCount("customer_kb"), 1);
}

TEST(ImportManagerTest, RowsLimitExceededRejected) {
    // Estimate returns rows_.size(); set a tiny cap to trip ROWS_LIMIT.
    ManagerFixture f({Row("1", "a"), Row("2", "b"), Row("3", "c")});
    ImportManagerConfig cfg;
    cfg.query_constraints.max_rows = 2;
    cfg.pg_host = "db.internal";
    cfg.pg_db = "crm";
    auto mgr = std::make_shared<ImportManager>(f.conn_mgr, f.query_exec, f.serializer, f.feeder,
                                               f.cleaner, f.task_store, f.oplog, cfg);
    auto r = mgr->StartImport(f.Req(), AdminCtx());
    EXPECT_FALSE(r.ok());
    EXPECT_NE(r.status().message().find("CX_ERR_IMPORT_ROWS_LIMIT_EXCEEDED"), std::string::npos);
}

// When EstimateRowCount is unavailable (standalone real executor returns a transient),
// the import still queues with rows_total unknown (0) — the estimate is a best-effort
// pre-check, not a hard gate (only an OVER-cap estimate blocks).
TEST(ImportManagerTest, ImportProceedsWhenEstimateUnavailable) {
    auto secret = std::make_shared<InMemorySecretStore>();
    auto conn_store = std::make_shared<InMemoryConnectionStore>();
    auto conn_mgr = std::make_shared<ConnectionManager>(secret, conn_store);
    auto ref = conn_mgr->Register("crm", "postgres://u:p@db.internal:5432/crm", "t1", "admin1");
    auto qexec = std::make_shared<QueryExecutor>();  // real: Estimate returns CONNECTION_FAILED
    ImportManagerConfig cfg;
    cfg.pg_host = "db.internal";
    cfg.pg_db = "crm";
    auto mgr = std::make_shared<ImportManager>(
        conn_mgr, qexec, std::make_shared<TextSerializer>(),
        std::make_shared<InMemorySpcFeeder>(), std::make_shared<InMemoryBlockCleaner>(),
        std::make_shared<InMemoryImportTaskStore>(), std::make_shared<FakeOpLogger>(), cfg);

    ImportRequest req;
    req.namespace_id = "ns";
    req.connection_ref = ref.value();
    req.query.table = "users";
    auto r = mgr->StartImport(req, AdminCtx());
    ASSERT_TRUE(r.ok());  // queued despite no estimate
    auto p = mgr->GetProgress(r.value());
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(p->rows_total, 0);  // unknown total
}

TEST(ImportManagerTest, CancelAfterStartRecordsCancelledMetric) {
    ImportMetrics::Instance().ResetForTest();
    ManagerFixture f({Row("1", "Ada")});
    auto r = f.mgr->StartImport(f.Req(), AdminCtx());
    ASSERT_TRUE(r.ok());
    f.mgr->Cancel(r.value());  // metric increment regardless of race outcome
    EXPECT_GE(ImportMetrics::Instance().ImportsCount(ImportMetrics::ImportOutcome::kCancelled), 1u);
    EXPECT_FALSE(f.mgr->Cancel("unknown_task"));  // unknown → false, no metric
}

// --- S2/S4 HTTP handler parse + response ---

TEST(ImportHandlerTest, ParseRejectsTemplateStrategy) {
    nlohmann::json body = {{"namespace", "ns"}, {"connection_ref", "db_conn_a"},
                           {"table", "users"}, {"text_strategy", "template"}};
    auto r = server::ImportHandler::ParseImportRequest(body);
    EXPECT_FALSE(r.ok());
}

TEST(ImportHandlerTest, ParseRequiresExactlyOneOfTableOrSql) {
    nlohmann::json neither = {{"namespace", "ns"}, {"connection_ref", "db_conn_a"}};
    EXPECT_FALSE(server::ImportHandler::ParseImportRequest(neither).ok());
    nlohmann::json both = {{"namespace", "ns"}, {"connection_ref", "db_conn_a"},
                           {"table", "users"}, {"sql", "SELECT 1"}};
    EXPECT_FALSE(server::ImportHandler::ParseImportRequest(both).ok());
    nlohmann::json table_ok = {{"namespace", "ns"}, {"connection_ref", "db_conn_a"},
                               {"table", "users"}, {"filter", {{"status", {{"eq", "active"}}}}},
                               {"columns", {"id", "name"}}};
    auto r = server::ImportHandler::ParseImportRequest(table_ok);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value().query.table, "users");
    EXPECT_EQ(r.value().query.columns.size(), 2u);
}

TEST(ImportHandlerTest, StartImportHandlerReturnsQueuedBody) {
    ManagerFixture f({Row("1", "Ada")});
    server::ImportHandler handler(f.mgr, f.conn_mgr);
    nlohmann::json body = {{"namespace", "customer_kb"}, {"connection_ref", f.ref},
                           {"table", "users"}};
    int http = 0;
    auto out = handler.HandleStartImport(body, AdminCtx(), http);
    EXPECT_EQ(http, 200);
    EXPECT_TRUE(out.contains("task_id"));
    EXPECT_EQ(out["status"], "queued");
    EXPECT_EQ(out["connection_ref"], f.ref);
    EXPECT_TRUE(out["error"].is_null());
    EXPECT_NE(out["progress_endpoint"].get<std::string>().find("/progress"), std::string::npos);
}

TEST(ImportHandlerTest, StartImportNonAdminGives403) {
    ManagerFixture f({Row("1", "Ada")});
    server::ImportHandler handler(f.mgr, f.conn_mgr);
    nlohmann::json body = {{"namespace", "ns"}, {"connection_ref", f.ref}, {"table", "users"}};
    AuthContext non_admin;
    non_admin.tenant_id = "t1";
    non_admin.permissions = kPermRead;
    int http = 0;
    auto out = handler.HandleStartImport(body, non_admin, http);
    EXPECT_EQ(http, 403);
    EXPECT_EQ(out["error"]["code"], "CX_ERR_IMPORT_AUTH_DENIED");
}

TEST(ImportHandlerTest, GetProgressUnknownGives404) {
    ManagerFixture f({Row("1", "Ada")});
    server::ImportHandler handler(f.mgr, f.conn_mgr);
    int http = 0;
    auto out = handler.HandleGetProgress("nope", http);
    EXPECT_EQ(http, 404);
    EXPECT_EQ(out["error"]["code"], "CX_ERR_IMPORT_TASK_NOT_FOUND");
}

TEST(ImportHandlerTest, RegisterConnectionReturnsRefNotDsn) {
    ManagerFixture f({Row("1", "Ada")});
    server::ImportHandler handler(f.mgr, f.conn_mgr);
    nlohmann::json body = {{"name", "crm2"},
                           {"dsn", "postgres://u:secret@h:5432/db"}};
    int http = 0;
    auto out = handler.HandleRegisterConnection(body, AdminCtx(), http);
    EXPECT_EQ(http, 200);
    EXPECT_TRUE(out.contains("ref_id"));
    EXPECT_FALSE(out.dump().find("secret") != std::string::npos);  // DSN never echoed
}

TEST(ImportHandlerTest, RegisterConnectionNonAdminAndMissingFields) {
    ManagerFixture f({Row("1", "Ada")});
    server::ImportHandler handler(f.mgr, f.conn_mgr);
    int http = 0;
    // non-admin → 403.
    AuthContext non_admin;
    non_admin.tenant_id = "t1";
    non_admin.permissions = kPermRead;
    auto a = handler.HandleRegisterConnection({{"name", "x"}, {"dsn", "d"}}, non_admin, http);
    EXPECT_EQ(http, 403);
    EXPECT_EQ(a["error"]["code"], "CX_ERR_IMPORT_AUTH_DENIED");
    // missing dsn → 400.
    auto b = handler.HandleRegisterConnection({{"name", "x"}}, AdminCtx(), http);
    EXPECT_EQ(http, 400);
    EXPECT_EQ(b["error"]["code"], "CX_ERR_IMPORT_INVALID_SQL");
}

TEST(ImportHandlerTest, ListConnectionsAdminAndNonAdmin) {
    ManagerFixture f({Row("1", "Ada")});  // fixture registered one ref already
    server::ImportHandler handler(f.mgr, f.conn_mgr);
    int http = 0;
    auto ok = handler.HandleListConnections(AdminCtx(), http);
    EXPECT_EQ(http, 200);
    ASSERT_TRUE(ok["connections"].is_array());
    EXPECT_GE(ok["connections"].size(), 1u);
    // never carries a dsn field.
    EXPECT_EQ(ok.dump().find("\"dsn\""), std::string::npos);

    AuthContext non_admin;
    non_admin.tenant_id = "t1";
    non_admin.permissions = kPermRead;
    auto denied = handler.HandleListConnections(non_admin, http);
    EXPECT_EQ(http, 403);
    EXPECT_EQ(denied["error"]["code"], "CX_ERR_IMPORT_AUTH_DENIED");
}

TEST(ImportHandlerTest, RevokeConnectionSuccessAndCrossTenant) {
    ManagerFixture f({Row("1", "Ada")});
    server::ImportHandler handler(f.mgr, f.conn_mgr);
    int http = 0;
    // success (owning tenant).
    auto ok = handler.HandleRevokeConnection(f.ref, {{"reason", "rotation"}}, AdminCtx("t1"), http);
    EXPECT_EQ(http, 200);
    EXPECT_EQ(ok["revoked"], true);
    EXPECT_EQ(ok["ref_id"], f.ref);
}

TEST(ImportHandlerTest, RevokeCrossTenantGives403) {
    ManagerFixture f({Row("1", "Ada")});
    server::ImportHandler handler(f.mgr, f.conn_mgr);
    int http = 0;
    auto denied = handler.HandleRevokeConnection(f.ref, {{"reason", "x"}}, AdminCtx("t2"), http);
    EXPECT_EQ(http, 403);
    EXPECT_EQ(denied["error"]["code"], "CX_ERR_IMPORT_CROSS_TENANT_REF");
}

TEST(ImportHandlerTest, CancelExistingTaskReturns200) {
    ManagerFixture f({Row("1", "Ada")});
    server::ImportHandler handler(f.mgr, f.conn_mgr);
    int http = 0;
    auto start = handler.HandleStartImport(
        {{"namespace", "customer_kb"}, {"connection_ref", f.ref}, {"table", "users"}}, AdminCtx(),
        http);
    ASSERT_EQ(http, 200);
    std::string task_id = start["task_id"].get<std::string>();
    auto out = handler.HandleCancel(task_id, http);
    EXPECT_EQ(http, 200);
    EXPECT_EQ(out["task_id"], task_id);
    EXPECT_TRUE(out.contains("status"));
}

TEST(ImportHandlerTest, CancelUnknownTaskGives404) {
    ManagerFixture f({Row("1", "Ada")});
    server::ImportHandler handler(f.mgr, f.conn_mgr);
    int http = 0;
    auto out = handler.HandleCancel("nope", http);
    EXPECT_EQ(http, 404);
    EXPECT_EQ(out["error"]["code"], "CX_ERR_IMPORT_TASK_NOT_FOUND");
}

TEST(ImportHandlerTest, ParseRejectsNonObjectAndMissingNamespace) {
    EXPECT_FALSE(server::ImportHandler::ParseImportRequest(nlohmann::json::array()).ok());
    EXPECT_FALSE(server::ImportHandler::ParseImportRequest({{"connection_ref", "r"},
                                                            {"table", "t"}})
                     .ok());  // no namespace
    EXPECT_FALSE(server::ImportHandler::ParseImportRequest({{"namespace", "n"}, {"table", "t"}})
                     .ok());  // no connection_ref
}

// A failed import's progress body embeds the error (via the real QueryExecutor
// which returns CONNECTION_FAILED in standalone — the live PG path is integration).
TEST(ImportHandlerTest, ProgressBodyForFailedImportHasErrorEnvelope) {
    // Use the *real* validating QueryExecutor so the worker fails on the PG seam.
    auto secret = std::make_shared<InMemorySecretStore>();
    auto conn_store = std::make_shared<InMemoryConnectionStore>();
    auto conn_mgr = std::make_shared<ConnectionManager>(secret, conn_store);
    auto ref = conn_mgr->Register("crm", "postgres://u:p@db.internal:5432/crm", "t1", "admin1");
    auto qexec = std::make_shared<QueryExecutor>();  // real: validates then CONNECTION_FAILED
    ImportManagerConfig cfg;
    cfg.pg_host = "db.internal";
    cfg.pg_db = "crm";
    auto mgr = std::make_shared<ImportManager>(
        conn_mgr, qexec, std::make_shared<TextSerializer>(),
        std::make_shared<InMemorySpcFeeder>(), std::make_shared<InMemoryBlockCleaner>(),
        std::make_shared<InMemoryImportTaskStore>(), std::make_shared<FakeOpLogger>(), cfg);
    server::ImportHandler handler(mgr, conn_mgr);

    int http = 0;
    auto start = handler.HandleStartImport(
        {{"namespace", "ns"}, {"connection_ref", ref.value()}, {"table", "users"}}, AdminCtx(),
        http);
    ASSERT_EQ(http, 200);
    std::string task_id = start["task_id"].get<std::string>();
    ASSERT_TRUE(WaitFor([&] {
        int h = 0;
        return handler.HandleGetProgress(task_id, h)["status"] == "failed";
    }));
    int h = 0;
    auto p = handler.HandleGetProgress(task_id, h);
    EXPECT_EQ(h, 200);
    EXPECT_EQ(p["status"], "failed");
    ASSERT_TRUE(p["error"].is_object());
    EXPECT_EQ(p["error"]["code"], "CX_ERR_IMPORT_CONNECTION_FAILED");
}

}  // namespace
}  // namespace cortrix::import
