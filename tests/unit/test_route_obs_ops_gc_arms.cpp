// Remaining error/validation-arm coverage for three route modules that already
// have a happy-path suite but leave the service-error / per-NS / re-inflation
// branches uncovered (SERVER_WAVE_GAPS.md). Deterministic in-process loopback
// httplib servers, 127.0.0.1 only, no LLM.
//
//   * OpsRoutesArms — operations_routes.cpp: the CX_ERR_OPLOG_* re-inflation arms
//     that a *real* OperationLogger never emits on a clean in-memory db
//     (CLEANUP_RUNNING / INTERNAL / UNAUTHORIZED-from-Query / pagination detail
//     with a non-numeric total). Driven by a fake IOperationLogger whose Query
//     returns a chosen CX_ERR_OPLOG_* token Status.
//   * GcRoutesArms — gc_routes.cpp: the route-level service-error arms (status /
//     run / purge / vacuum / reindex / restore) that surface a GcManager or
//     DocumentGcSweeper failure through WriteJsonError. Two fixtures: one with a
//     schemaless catalog db (catalog-layer fails) and one with a router that
//     errors (document-layer sweeper fails while the catalog succeeds).
//   * ObsRoutesArms — observability_routes.cpp TC4 wiring: the GLOBAL /traces
//     owner-resolver success path (agent_trace row + per-NS interaction_log owner
//     lookup), the per-NS namespace-not-found arm, and the per-NS list filter
//     validation arm — none of which the empty-db TC4 suite reaches.
//
// Suite prefixes (OpsRoutesArms / GcRoutesArms* / ObsRoutesArms) are globally
// unique. ApiKeyAuth is non-copyable → built in place, passed by reference.
#include <gtest/gtest.h>

#include <sqlite3.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "httplib.h"
#include <nlohmann/json.hpp>

#include "cortrix/auth/api_key_auth.h"
#include "cortrix/auth/auth_context.h"
#include "cortrix/catalog/batch_result.h"
#include "cortrix/catalog/catalog_db.h"
#include "cortrix/catalog/gc/blob_gc_sink.h"
#include "cortrix/catalog/gc/document_gc_sweeper.h"
#include "cortrix/catalog/gc/gc_manager.h"
#include "cortrix/catalog/schema_provider.h"
#include "cortrix/common/in_memory_global_config.h"
#include "cortrix/config/config.h"
#include "cortrix/memory/memory_store.h"
#include "cortrix/observability/operation_logger.h"
#include "cortrix/resource/namespace_facade.h"
#include "cortrix/agent_trace/agent_trace_schema.h"
#include "cortrix/server/routes/gc_routes.h"
#include "cortrix/server/routes/observability_routes.h"
#include "cortrix/server/routes/operations_routes.h"

#include "ns_pool_test_helper.h"

namespace cortrix {
namespace {

using json = nlohmann::json;
using ::testing::_;
using ::testing::Invoke;

int ArmsPort(int base) {
    static std::atomic<unsigned> ctr{0};
    return base + ((getpid() + ctr.fetch_add(1)) % 200);
}

static ApiKeyConfig MakeKey(const std::string& plaintext, const std::string& tenant, int perms) {
    ApiKeyConfig k;
    k.key_hash = ApiKeyAuth::HashKey(plaintext);
    k.tenant_id = tenant;  // -> AuthContext.user_id (MVP user_id == tenant_id)
    k.permissions = perms;
    return k;
}

// ===========================================================================
// OpsRoutesArms — operations_routes.cpp CX_ERR_OPLOG_* re-inflation arms.
// ===========================================================================

// A fake logger whose Query returns a caller-chosen error Status (CX_ERR_OPLOG_*
// token), so the route's re-inflation switch (operations_routes.cpp §298-313) can be
// driven to every arm a clean in-memory OperationLogger never reaches.
class FakeOplogLogger : public observability::IOperationLogger {
public:
    void SetQueryError(StatusCode code, std::string message) {
        err_code_ = code;
        err_msg_ = std::move(message);
        fail_ = true;
    }
    void Log(const observability::OperationLogEntry&,
             const observability::TraceContext*) override {}
    void BatchLog(const std::vector<observability::OperationLogEntry>&,
                  const observability::TraceContext*) override {}
    Result<observability::OperationLogQueryResult> Query(
        const observability::OperationLogFilter&,
        const observability::TraceContext*) override {
        if (fail_) return Status(err_code_, err_msg_);
        return observability::OperationLogQueryResult{};
    }
    void Cleanup() override {}
    observability::OperationLogStats GetStats() override { return {}; }
    observability::HealthStatus Health() override { return {}; }

private:
    bool fail_ = false;
    StatusCode err_code_ = StatusCode::kInternal;
    std::string err_msg_;
};

class OpsRoutesArms : public ::testing::Test {
protected:
    void SetUp() override {
        logger_ = std::make_unique<FakeOplogLogger>();
        auth_ = std::make_unique<ApiKeyAuth>();
        admin_key_ = "ops-arms-admin-key";
        // admin so cross-user / param paths are reachable; the fake Query decides the arm.
        auth_->LoadKeys({MakeKey(admin_key_, "admin-user", kPermRead | kPermAdmin)});

        server_ = std::make_unique<httplib::Server>();
        RegisterOperationsRoutes(*server_, *logger_, *auth_);
        port_ = ArmsPort(19010);
        server_thread_ = std::thread([this] { server_->listen("127.0.0.1", port_); });
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }
    void TearDown() override {
        if (server_) server_->stop();
        if (server_thread_.joinable()) server_thread_.join();
        logger_.reset();
    }
    httplib::Headers Admin() { return {{"Authorization", "Bearer " + admin_key_}}; }

    std::unique_ptr<FakeOplogLogger> logger_;
    std::unique_ptr<ApiKeyAuth> auth_;
    std::unique_ptr<httplib::Server> server_;
    std::thread server_thread_;
    int port_ = 0;
    std::string admin_key_;
};

// CLEANUP_RUNNING from Query → retry_at_ms (base + jitter) structured_data arm.
TEST_F(OpsRoutesArms, QueryCleanupRunningReInflatesRetryAt) {
    logger_->SetQueryError(StatusCode::kUnavailable,
                           "CX_ERR_OPLOG_CLEANUP_RUNNING: a cleanup pass is in progress");
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/operations", Admin());
    ASSERT_TRUE(res);
    auto body = json::parse(res->body);
    EXPECT_EQ(body["error"]["code"], "CX_ERR_OPLOG_CLEANUP_RUNNING");
    ASSERT_TRUE(body["error"]["structured_data"].contains("retry_at_ms"));
    EXPECT_GE(body["error"]["structured_data"]["retry_at_ms"].get<int>(), 5000);
}

// An unrecognized CX_ERR_OPLOG_* token → kInternal arm (error_id correlation tag).
TEST_F(OpsRoutesArms, QueryUnknownTokenReInflatesInternal) {
    logger_->SetQueryError(StatusCode::kInternal,
                           "CX_ERR_OPLOG_SOMETHING_NEW: an unmapped fault");
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/operations", Admin());
    ASSERT_TRUE(res);
    auto body = json::parse(res->body);
    EXPECT_EQ(body["error"]["code"], "CX_ERR_OPLOG_INTERNAL");
    ASSERT_TRUE(body["error"]["structured_data"].contains("error_id"));
    EXPECT_FALSE(body["error"]["structured_data"]["error_id"].get<std::string>().empty());
}

// UNAUTHORIZED surfaced by Query itself (not the route's pre-check) → required_role arm.
TEST_F(OpsRoutesArms, QueryUnauthorizedReInflatesRequiredRole) {
    logger_->SetQueryError(StatusCode::kPermissionDenied,
                           "CX_ERR_OPLOG_UNAUTHORIZED: cross-user query denied");
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/operations", Admin());
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 403) << res->body;
    auto body = json::parse(res->body);
    EXPECT_EQ(body["error"]["code"], "CX_ERR_OPLOG_UNAUTHORIZED");
    EXPECT_EQ(body["error"]["structured_data"]["required_role"], "admin");
}

// PAGINATION_OUT_OF_RANGE whose detail has a NON-numeric total → ParseTrailingTotalCount
// catch returns nullopt → total_count defaults to 0. Also exercises the trace_id filter param.
TEST_F(OpsRoutesArms, QueryPaginationNonNumericTotalDefaultsZero) {
    logger_->SetQueryError(StatusCode::kInvalidArgument,
                           "CX_ERR_OPLOG_PAGINATION_OUT_OF_RANGE: offset 5 >= total_count notanumber");
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/operations?offset=5&trace_id=tr-1", Admin());
    ASSERT_TRUE(res);
    auto body = json::parse(res->body);
    EXPECT_EQ(body["error"]["code"], "CX_ERR_OPLOG_PAGINATION_OUT_OF_RANGE");
    EXPECT_EQ(body["error"]["structured_data"]["offset"], 5);
    EXPECT_EQ(body["error"]["structured_data"]["total_count"], 0);
}

// ===========================================================================
// GcRoutesArms — gc_routes.cpp route-level service-error arms.
// ===========================================================================

// Fixture A: a SCHEMALESS catalog db so the GcManager (catalog layer) fails on its
// first prepare. The sweeper's router is wired ok but is never reached because the
// catalog op runs (and fails) first.
class GcRoutesArmsCatalogFail : public ::testing::Test {
protected:
    void SetUp() override {
        // Bare in-memory db, NO catalog schema (do not use CatalogDb::Open which migrates).
        ASSERT_EQ(sqlite3_open(":memory:", &raw_db_), SQLITE_OK);
        gc_ = std::make_unique<catalog::gc::GcManager>(raw_db_, &sink_, cfg_);

        harness_ = std::make_unique<cortrix::test::NsPoolHarness>(
            std::filesystem::temp_directory_path() /
            ("gcarms_cat_" + std::to_string(reinterpret_cast<uintptr_t>(this))));
        harness_->Admit(kNs);
        ON_CALL(router_, ListNamespaces(_))
            .WillByDefault(Invoke([](const catalog::ListNamespacesOptions&) {
                catalog::BatchResult<std::string> r;
                r.results = {kNs};
                return cortrix::Result<catalog::BatchResult<std::string>>(r);
            }));
        sweeper_ = std::make_unique<catalog::gc::DocumentGcSweeper>(
            &router_, &harness_->ipool(), cfg_);

        auth_ = std::make_unique<ApiKeyAuth>();
        admin_key_ = "gcarms-cat-admin";
        auth_->LoadKeys({MakeKey(admin_key_, "admin-user", kPermRead | kPermWrite | kPermAdmin)});

        server_ = std::make_unique<httplib::Server>();
        RegisterGcRoutes(*server_, *gc_, *sweeper_, *auth_);
        port_ = ArmsPort(19050);
        server_thread_ = std::thread([this] { server_->listen("127.0.0.1", port_); });
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }
    void TearDown() override {
        if (server_) server_->stop();
        if (server_thread_.joinable()) server_thread_.join();
        gc_.reset();
        if (raw_db_) sqlite3_close(raw_db_);
    }
    httplib::Headers Admin() { return {{"Authorization", "Bearer " + admin_key_}}; }
    httplib::Headers AdminConfirm() {
        return {{"Authorization", "Bearer " + admin_key_}, {"X-Ops-Confirm", "true"}};
    }

    static constexpr const char* kNs = "sales";
    sqlite3* raw_db_ = nullptr;
    catalog::gc::NullBlobGcSink sink_;
    GcConfig cfg_;
    std::unique_ptr<catalog::gc::GcManager> gc_;
    std::unique_ptr<cortrix::test::NsPoolHarness> harness_;
    ::testing::NiceMock<cortrix::test::MockNSRouter> router_;
    std::unique_ptr<catalog::gc::DocumentGcSweeper> sweeper_;
    std::unique_ptr<ApiKeyAuth> auth_;
    std::unique_ptr<httplib::Server> server_;
    std::thread server_thread_;
    int port_ = 0;
    std::string admin_key_;
};

// GET /gc/status → GcManager::GetStatus fails (no file_locations table) → 5xx envelope.
TEST_F(GcRoutesArmsCatalogFail, StatusCatalogFailureSurfacesError) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/gc/status", Admin());
    ASSERT_TRUE(res);
    EXPECT_GE(res->status, 500) << res->body;
    EXPECT_TRUE(json::parse(res->body).contains("error"));
}

// POST /gc/run (confirmed) → gc.RunOnce fails on the schemaless catalog → error.
TEST_F(GcRoutesArmsCatalogFail, RunCatalogFailureSurfacesError) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Post("/api/v1/gc/run", AdminConfirm(), "", "application/json");
    ASSERT_TRUE(res);
    EXPECT_GE(res->status, 500) << res->body;
    EXPECT_TRUE(json::parse(res->body).contains("error"));
}

// POST /gc/purge (confirmed) → gc.Purge fails on the schemaless catalog → error.
TEST_F(GcRoutesArmsCatalogFail, PurgeCatalogFailureSurfacesError) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Post("/api/v1/gc/purge", AdminConfirm(), "", "application/json");
    ASSERT_TRUE(res);
    EXPECT_GE(res->status, 500) << res->body;
    EXPECT_TRUE(json::parse(res->body).contains("error"));
}

// Fixture B: a REAL catalog (so the GcManager layer succeeds) + a router that errors,
// so the DocumentGcSweeper layer fails. This reaches the sweeper-failure arms that run
// AFTER the catalog op succeeds (status soft-count, run/purge document layer, restore).
class GcRoutesArmsSweeperFail : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(catalog_.Open(":memory:").ok());
        ASSERT_EQ(sqlite3_exec(catalog_.db(),
            "INSERT INTO tenants(tenant_id, created_at) VALUES('t1', 0);"
            "INSERT INTO units(unit_id, tenant_id, created_at) VALUES('u1','t1',0);",
            nullptr, nullptr, nullptr), SQLITE_OK);
        gc_ = std::make_unique<catalog::gc::GcManager>(catalog_.db(), &sink_, cfg_);

        harness_ = std::make_unique<cortrix::test::NsPoolHarness>(
            std::filesystem::temp_directory_path() /
            ("gcarms_swp_" + std::to_string(reinterpret_cast<uintptr_t>(this))));
        harness_->Admit(kNs);
        // The router errors → every sweeper op (CountSoftDeleted / RunOnce / Purge /
        // Restore) fails at its ListNamespaces() call.
        ON_CALL(router_, ListNamespaces(_))
            .WillByDefault(Invoke([](const catalog::ListNamespacesOptions&) {
                return cortrix::Result<catalog::BatchResult<std::string>>(
                    Status::Internal("CX_ERR_GC_INTERNAL: ns router unavailable"));
            }));
        sweeper_ = std::make_unique<catalog::gc::DocumentGcSweeper>(
            &router_, &harness_->ipool(), cfg_);

        auth_ = std::make_unique<ApiKeyAuth>();
        admin_key_ = "gcarms-swp-admin";
        auth_->LoadKeys({MakeKey(admin_key_, "admin-user", kPermRead | kPermWrite | kPermAdmin)});

        server_ = std::make_unique<httplib::Server>();
        RegisterGcRoutes(*server_, *gc_, *sweeper_, *auth_);
        port_ = ArmsPort(19080);
        server_thread_ = std::thread([this] { server_->listen("127.0.0.1", port_); });
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }
    void TearDown() override {
        if (server_) server_->stop();
        if (server_thread_.joinable()) server_thread_.join();
        gc_.reset();
    }
    httplib::Headers Admin() { return {{"Authorization", "Bearer " + admin_key_}}; }
    httplib::Headers AdminConfirm() {
        return {{"Authorization", "Bearer " + admin_key_}, {"X-Ops-Confirm", "true"}};
    }

    static constexpr const char* kNs = "sales";
    catalog::CatalogDb catalog_;
    catalog::gc::NullBlobGcSink sink_;
    GcConfig cfg_;
    std::unique_ptr<catalog::gc::GcManager> gc_;
    std::unique_ptr<cortrix::test::NsPoolHarness> harness_;
    ::testing::NiceMock<cortrix::test::MockNSRouter> router_;
    std::unique_ptr<catalog::gc::DocumentGcSweeper> sweeper_;
    std::unique_ptr<ApiKeyAuth> auth_;
    std::unique_ptr<httplib::Server> server_;
    std::thread server_thread_;
    int port_ = 0;
    std::string admin_key_;
};

// GET /gc/status → catalog GetStatus ok, then sweeper.CountSoftDeleted fails → error.
TEST_F(GcRoutesArmsSweeperFail, StatusSweeperSoftCountFailureSurfacesError) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/gc/status", Admin());
    ASSERT_TRUE(res);
    EXPECT_GE(res->status, 500) << res->body;
    EXPECT_TRUE(json::parse(res->body).contains("error"));
}

// POST /gc/run (confirmed) → catalog RunOnce ok, sweeper.RunOnce fails → error.
TEST_F(GcRoutesArmsSweeperFail, RunSweeperFailureSurfacesError) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Post("/api/v1/gc/run", AdminConfirm(), "", "application/json");
    ASSERT_TRUE(res);
    EXPECT_GE(res->status, 500) << res->body;
    EXPECT_TRUE(json::parse(res->body).contains("error"));
}

// POST /gc/purge (confirmed) → catalog Purge ok, sweeper.Purge fails → error.
TEST_F(GcRoutesArmsSweeperFail, PurgeSweeperFailureSurfacesError) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Post("/api/v1/gc/purge", AdminConfirm(), "", "application/json");
    ASSERT_TRUE(res);
    EXPECT_GE(res->status, 500) << res->body;
    EXPECT_TRUE(json::parse(res->body).contains("error"));
}

// POST /gc/restore with a well-formed body → sweeper.Restore fails → error (not the
// partial-success 200; the restore service-error arm).
TEST_F(GcRoutesArmsSweeperFail, RestoreSweeperFailureSurfacesError) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Post("/api/v1/gc/restore", Admin(),
                        R"({"document_ids":["docA"]})", "application/json");
    ASSERT_TRUE(res);
    EXPECT_GE(res->status, 500) << res->body;
    EXPECT_TRUE(json::parse(res->body).contains("error"));
}

// ===========================================================================
// ObsRoutesArms — observability_routes.cpp TC4 wiring arms.
// ===========================================================================

// TC4 split: GLOBAL /traces (RegisterTracesRoutesGlobal over a real global agent_trace
// db) + PER-NS interactions (RegisterInteractionsRoutesPerNs over the F05 pool). We seed
// a global agent_trace row (session touching NS "default") AND a per-NS interaction_log
// row mapping that session to its owner, so the owner-resolver success path runs.
class ObsRoutesArms : public ::testing::Test {
protected:
    void SetUp() override {
        tmp_dir_ = (std::filesystem::temp_directory_path() /
                    ("obs_arms_" + std::to_string(getpid()) + "_" +
                     std::to_string(reinterpret_cast<uintptr_t>(this)))).string();
        std::filesystem::create_directories(tmp_dir_);

        // Auth: owner key (user_id == "alice") read-only + admin key.
        auth_ = std::make_unique<ApiKeyAuth>();
        owner_key_ = "obs-arms-owner";
        admin_key_ = "obs-arms-admin";
        auth_->LoadKeys({
            MakeKey(owner_key_, "alice", kPermRead),
            MakeKey(admin_key_, "root", kPermRead | kPermAdmin),
        });

        harness_ = std::make_unique<cortrix::test::NsPoolHarness>(tmp_dir_ + "/pool");
        ASSERT_TRUE(harness_->Admit(kNs).ok());

        global_config_ = std::make_shared<InMemoryGlobalConfig>();

        // Global agent_trace db (TC4) migrated with the F13 provider, then seeded with
        // a row for "alice-sess" stamped namespace_id = kNs.
        ASSERT_EQ(sqlite3_open((tmp_dir_ + "/global.db").c_str(), &global_db_), SQLITE_OK);
        {
            cortrix::catalog::SchemaMigrator m;
            cortrix::agent_trace::AgentTraceSchemaProvider at_provider;
            m.Register(&at_provider);
            ASSERT_TRUE(m.MigrateCatalog(global_db_).ok());
        }
        SeedTrace("alice-sess", kNs, "cortrix_query", 1000);
        SeedTrace("alice-sess", kNs, "cortrix_query", 1001);

        // Per-NS interaction_log row maps "alice-sess" -> user "alice" (the owner
        // resolver reads interaction_log first). Write directly to the NS memory.db.
        {
            cortrix::resource::NamespaceFacade facade(harness_->ipool(), kNs);
            ASSERT_TRUE(facade.Acquire().ok());
            sqlite3* mem = facade.memory().db_handle();
            ASSERT_NE(mem, nullptr);
            const std::string sql =
                std::string(
                    "INSERT INTO interaction_log(id, session_id, namespace_name, user_id, role, content) "
                    "VALUES('int-1','alice-sess','") +
                kNs + "','alice','user','hi');";
            char* err = nullptr;
            ASSERT_EQ(sqlite3_exec(mem, sql.c_str(), nullptr, nullptr, &err), SQLITE_OK)
                << (err ? err : "");
            sqlite3_free(err);
        }

        server_ = std::make_unique<httplib::Server>();
        RegisterTracesRoutesGlobal(*server_, global_db_, harness_->ipool(), global_config_, *auth_);
        RegisterInteractionsRoutesPerNs(*server_, harness_->ipool(), *auth_);
        port_ = ArmsPort(19110);
        server_thread_ = std::thread([this] { server_->listen("127.0.0.1", port_); });

        httplib::Client cli("127.0.0.1", port_);
        for (int i = 0; i < 50; ++i) {
            auto res = cli.Get("/api/v1/interactions?namespace_id=" + std::string(kNs), Owner());
            if (res) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    void TearDown() override {
        if (server_) server_->stop();
        if (server_thread_.joinable()) server_thread_.join();
        harness_.reset();
        if (global_db_) sqlite3_close(global_db_);
        std::error_code ec;
        std::filesystem::remove_all(tmp_dir_, ec);
    }

    void SeedTrace(const std::string& session, const std::string& ns,
                   const std::string& method, int64_t created_at) {
        sqlite3_stmt* s = nullptr;
        ASSERT_EQ(sqlite3_prepare_v2(
                      global_db_,
                      "INSERT INTO agent_trace(session_id, method, source, duration_ms, "
                      "status, namespace_id, created_at) VALUES(?,?,?,?,?,?,?)",
                      -1, &s, nullptr),
                  SQLITE_OK);
        sqlite3_bind_text(s, 1, session.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 2, method.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 3, "mcp", -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(s, 4, 10);
        sqlite3_bind_text(s, 5, "success", -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 6, ns.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(s, 7, created_at);
        ASSERT_EQ(sqlite3_step(s), SQLITE_DONE);
        sqlite3_finalize(s);
    }

    httplib::Headers Owner() { return {{"Authorization", "Bearer " + owner_key_}}; }
    httplib::Headers Admin() { return {{"Authorization", "Bearer " + admin_key_}}; }

    static constexpr const char* kNs = "default";
    std::string tmp_dir_;
    std::unique_ptr<ApiKeyAuth> auth_;
    std::unique_ptr<cortrix::test::NsPoolHarness> harness_;
    std::shared_ptr<InMemoryGlobalConfig> global_config_;
    sqlite3* global_db_ = nullptr;
    std::unique_ptr<httplib::Server> server_;
    std::thread server_thread_;
    int port_ = 0;
    std::string owner_key_, admin_key_;
};

// GLOBAL /traces with the owner key: the resolver reads agent_trace.namespace_id, then
// looks up the owner in that NS's interaction_log → "alice" matches the caller → 200
// with the seeded trace rows serialized (TraceEntryToJson over the global path).
TEST_F(ObsRoutesArms, GlobalTraceOwnerResolvedReadsSession) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/traces/alice-sess", Owner());
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 200) << res->body;
    auto body = json::parse(res->body);
    EXPECT_EQ(body["session_id"], "alice-sess");
    EXPECT_EQ(body["trace_count"], 2);
    ASSERT_EQ(body["traces"].size(), 2u);
    EXPECT_EQ(body["traces"][0]["method"], "cortrix_query");
    EXPECT_EQ(body["traces"][0]["source"], "mcp");
    EXPECT_TRUE(body["traces"][0]["error_code"].is_null());  // optional column → null
}

// A non-owner, non-admin reading the resolved session → cross-user denied (the §8.1
// anti-leak arm over the global resolver path).
TEST_F(ObsRoutesArms, GlobalTraceNonOwnerDenied) {
    auth_->LoadKeys({
        MakeKey(owner_key_, "alice", kPermRead),
        MakeKey(admin_key_, "root", kPermRead | kPermAdmin),
        MakeKey("obs-arms-mallory", "mallory", kPermRead),
    });
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/traces/alice-sess",
                       {{"Authorization", "Bearer obs-arms-mallory"}});
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 403) << res->body;
    EXPECT_EQ(json::parse(res->body)["error"]["code"], "CX_ERR_F13_UNAUTHORIZED");
}

// PER-NS /interactions/:id/sources with a namespace that does not exist → the with_ns
// Acquire() fails → WriteF13Error SESSION_NOT_FOUND (the per-NS not-found arm).
TEST_F(ObsRoutesArms, SourcesUnknownNamespaceIsNotFound) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/interactions/int-1/sources?namespace=ghost-ns", Owner());
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404) << res->body;
    EXPECT_EQ(json::parse(res->body)["error"]["code"], "CX_ERR_F13_SESSION_NOT_FOUND");
}

// PER-NS /interactions/:id/sources with the real namespace → ServeSources runs over the
// NS memory.db. No sources for this interaction → handler NOT_FOUND (404), exercising
// the per-NS ServeSources body + its error rendering.
TEST_F(ObsRoutesArms, SourcesValidNamespaceRunsHandler) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/interactions/int-1/sources?namespace=" + std::string(kNs),
                       Owner());
    ASSERT_TRUE(res);
    EXPECT_LT(res->status, 500) << res->body;  // reached the handler, not a server fault
}

// PER-NS /interactions with a non-integer timestamp filter → ServeListInteractions
// validation arm → 400 INVALID_FILTER (the per-NS list filter path).
TEST_F(ObsRoutesArms, ListInteractionsBadTimestampIsInvalidFilter) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/interactions?namespace_id=" + std::string(kNs) +
                       "&from_timestamp=notanumber", Owner());
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400) << res->body;
    EXPECT_EQ(json::parse(res->body)["error"]["code"], "CX_ERR_F13_INVALID_FILTER");
}

}  // namespace
}  // namespace cortrix
