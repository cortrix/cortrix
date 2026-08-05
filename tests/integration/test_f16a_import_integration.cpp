#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <mutex>
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

// DB import standalone integration: drives the 9-issue decision matrix (DB import D1-D9)
// end-to-end through the composed stack (ConnectionManager + QueryExecutor +
// TextSerializer + SPC feed + BlockCleaner + ImportTaskQueue + ImportManager + the
// HTTP handler), with operation_log + metrics observed. Real PG / SPCPipeline /
// server routing are mocked → D3.5.
namespace cortrix::import {
namespace {

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
        const observability::OperationLogFilter&, const observability::TraceContext*) override {
        return observability::OperationLogQueryResult{};
    }
    void Cleanup() override {}
    observability::OperationLogStats GetStats() override { return {}; }
    observability::HealthStatus Health() override { return {}; }
    std::vector<std::string> actions() {
        std::lock_guard<std::mutex> lk(mu_);
        std::vector<std::string> a;
        for (auto& e : entries_) a.push_back(e.action);
        return a;
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

AuthContext Admin(const std::string& tenant = "t1", const std::string& user = "admin1") {
    AuthContext c;
    c.tenant_id = tenant;
    c.user_id = user;
    c.permissions = kPermAdmin | kPermRead | kPermWrite;
    return c;
}

template <typename Pred>
bool WaitFor(Pred pred, int timeout_ms = 3000) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return pred();
}

struct Stack {
    std::shared_ptr<FakeOpLogger> oplog = std::make_shared<FakeOpLogger>();
    std::shared_ptr<ConnectionManager> conn_mgr;
    std::shared_ptr<InMemorySpcFeeder> feeder = std::make_shared<InMemorySpcFeeder>();
    std::shared_ptr<InMemoryBlockCleaner> cleaner = std::make_shared<InMemoryBlockCleaner>();
    std::shared_ptr<ImportManager> mgr;
    std::shared_ptr<server::ImportHandler> handler;

    explicit Stack(std::vector<DbRow> rows) {
        auto secret = std::make_shared<InMemorySecretStore>();
        auto store = std::make_shared<InMemoryConnectionStore>();
        conn_mgr = std::make_shared<ConnectionManager>(secret, store, oplog);  // D1 + S6 audit
        auto qexec = std::make_shared<FakeQueryExecutor>(std::move(rows));
        auto serializer = std::make_shared<TextSerializer>();
        auto task_store = std::make_shared<InMemoryImportTaskStore>();
        ImportManagerConfig cfg;
        cfg.pg_host = "db.internal";
        cfg.pg_port = 5432;
        cfg.pg_db = "crm";
        mgr = std::make_shared<ImportManager>(conn_mgr, qexec, serializer, feeder, cleaner,
                                              task_store, oplog, cfg);
        handler = std::make_shared<server::ImportHandler>(mgr, conn_mgr);
    }
};

// Full happy path through the HTTP handler: D1 register → D2/D4/D5/D6 import →
// progress → completed. Asserts D1/D5/D6/D9 + S6 (all 3 oplog actions).
TEST(F16aIntegration, RegisterThenImportEndToEndViaHandler) {
    ImportMetrics::Instance().ResetForTest();
    Stack s({Row("1", "Ada"), Row("2", "Bob"), Row("3", "Cy")});

    // D1: register a connection (admin API).
    int http = 0;
    auto reg = s.handler->HandleRegisterConnection(
        {{"name", "crm"}, {"dsn", "postgres://u:p@db.internal:5432/crm"}}, Admin(), http);
    ASSERT_EQ(http, 200) << reg.dump();
    std::string ref = reg["ref_id"].get<std::string>();

    // D6: start import.
    auto start = s.handler->HandleStartImport(
        {{"namespace", "customer_kb"}, {"connection_ref", ref}, {"table", "users"}}, Admin(), http);
    ASSERT_EQ(http, 200) << start.dump();
    std::string task_id = start["task_id"].get<std::string>();
    EXPECT_EQ(start["estimated_rows"], 3);  // R5 COUNT(*) pre-estimate flowed through

    // D6: poll progress to completion.
    ASSERT_TRUE(WaitFor([&] {
        int h = 0;
        auto p = s.handler->HandleGetProgress(task_id, h);
        return h == 200 && p["status"] == "completed";
    }));

    // D5: the 3 rows were fed to the SPC pipeline.
    EXPECT_EQ(s.feeder->total_chunks(), 3);

    // S6 (F16a-rev-6): all 3 operation_log actions recorded.
    auto acts = s.oplog->actions();
    EXPECT_NE(std::find(acts.begin(), acts.end(), "db_connection_register"), acts.end());
    EXPECT_NE(std::find(acts.begin(), acts.end(), "database_import"), acts.end());

    // D1 revoke → 3rd action.
    auto rev = s.handler->HandleRevokeConnection(ref, {{"reason", "rotation"}}, Admin(), http);
    ASSERT_EQ(http, 200) << rev.dump();
    acts = s.oplog->actions();
    EXPECT_NE(std::find(acts.begin(), acts.end(), "db_connection_revoke"), acts.end());

    // metrics: 1 success import + 3 rows.
    EXPECT_EQ(ImportMetrics::Instance().ImportsCount(ImportMetrics::ImportOutcome::kSuccess), 1u);
    EXPECT_EQ(ImportMetrics::Instance().RowsImportedTotal(), 3u);
}

// D2 security: an INSERT/UPDATE custom-SQL is rejected with CX_ERR_IMPORT_INVALID_SQL.
TEST(F16aIntegration, D2WriteSqlRejected) {
    Stack s({Row("1", "Ada")});
    int http = 0;
    auto reg = s.handler->HandleRegisterConnection(
        {{"name", "crm"}, {"dsn", "postgres://u:p@db.internal:5432/crm"}}, Admin(), http);
    std::string ref = reg["ref_id"].get<std::string>();
    auto out = s.handler->HandleStartImport(
        {{"namespace", "ns"}, {"connection_ref", ref}, {"sql", "DELETE FROM users"}}, Admin(), http);
    EXPECT_EQ(http, 400);
    EXPECT_EQ(out["error"]["code"], "CX_ERR_IMPORT_INVALID_SQL");
}

// D7 cross-tenant: tenant t2 cannot use t1's ref (CX_ERR_IMPORT_CROSS_TENANT_REF, 403).
TEST(F16aIntegration, D7CrossTenantRefRejected) {
    Stack s({Row("1", "Ada")});
    int http = 0;
    auto reg = s.handler->HandleRegisterConnection(
        {{"name", "crm"}, {"dsn", "postgres://u:p@db.internal:5432/crm"}}, Admin("t1"), http);
    std::string ref = reg["ref_id"].get<std::string>();
    auto out = s.handler->HandleStartImport(
        {{"namespace", "ns"}, {"connection_ref", ref}, {"table", "users"}}, Admin("t2", "evil"),
        http);
    EXPECT_EQ(http, 403);
    EXPECT_EQ(out["error"]["code"], "CX_ERR_IMPORT_CROSS_TENANT_REF");
}

// D3 full-overwrite: a re-import of the same table clears the prior Blocks first.
TEST(F16aIntegration, D3FullOverwriteOnReimport) {
    Stack s({Row("1", "Ada")});
    s.cleaner->SeedBlock("customer_kb", "postgres://db.internal:5432/crm/users/old");
    s.cleaner->SeedBlock("customer_kb", "postgres://db.internal:5432/crm/orders/keep");

    int http = 0;
    auto reg = s.handler->HandleRegisterConnection(
        {{"name", "crm"}, {"dsn", "postgres://u:p@db.internal:5432/crm"}}, Admin(), http);
    std::string ref = reg["ref_id"].get<std::string>();
    auto start = s.handler->HandleStartImport(
        {{"namespace", "customer_kb"}, {"connection_ref", ref}, {"table", "users"}}, Admin(), http);
    std::string task_id = start["task_id"].get<std::string>();
    ASSERT_TRUE(WaitFor([&] {
        int h = 0;
        return s.handler->HandleGetProgress(task_id, h)["status"] == "completed";
    }));
    // prior users Block cleared (D3); orders Block survives.
    EXPECT_EQ(s.cleaner->RemainingCount("customer_kb"), 1);
}

// D6 cancel: a cancel resolves to status=cancelled (not an error).
TEST(F16aIntegration, D6CancelResolvesToCancelled) {
    Stack s({Row("1", "Ada")});
    int http = 0;
    auto reg = s.handler->HandleRegisterConnection(
        {{"name", "crm"}, {"dsn", "postgres://u:p@db.internal:5432/crm"}}, Admin(), http);
    std::string ref = reg["ref_id"].get<std::string>();
    auto start = s.handler->HandleStartImport(
        {{"namespace", "ns"}, {"connection_ref", ref}, {"table", "users"}}, Admin(), http);
    std::string task_id = start["task_id"].get<std::string>();

    // cancel (may race completion; either way it must be a terminal, non-error state).
    s.handler->HandleCancel(task_id, http);
    EXPECT_EQ(http, 200);
    ASSERT_TRUE(WaitFor([&] {
        int h = 0;
        auto p = s.handler->HandleGetProgress(task_id, h);
        std::string st = p["status"];
        return st == "cancelled" || st == "completed";
    }));
    int h = 0;
    auto p = s.handler->HandleGetProgress(task_id, h);
    EXPECT_TRUE(p["error"].is_null());  // §5.4 note: cancel/complete carry no error body
}

}  // namespace
}  // namespace cortrix::import
