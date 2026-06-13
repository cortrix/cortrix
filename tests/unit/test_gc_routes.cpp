#include <gtest/gtest.h>

#include <sqlite3.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

#include "httplib.h"
#include <nlohmann/json.hpp>

#include "cortrix/auth/api_key_auth.h"
#include "cortrix/auth/auth_context.h"
#include "cortrix/catalog/batch_result.h"
#include "cortrix/catalog/catalog_db.h"
#include "cortrix/catalog/gc/blob_gc_sink.h"
#include "cortrix/catalog/gc/document_gc_sweeper.h"
#include "cortrix/catalog/gc/gc_manager.h"
#include "cortrix/common/data_types.h"
#include "cortrix/config/config.h"
#include "cortrix/resource/namespace_facade.h"
#include "cortrix/server/routes/gc_routes.h"
#include "cortrix/store/cortrix_store.h"

#include "ns_pool_test_helper.h"

// OPEN-2 GC routes: GcStatus contract shape, X-Ops-Confirm gating, restore
// PartialSuccessById, admin (kPermAdmin) gating, maintenance vacuum/reindex.
namespace cortrix {
namespace {

using json = nlohmann::json;
using ::testing::_;
using ::testing::Invoke;

class GcRoutesTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(catalog_.Open(":memory:").ok());
        ASSERT_EQ(sqlite3_exec(catalog_.db(),
            "INSERT INTO tenants(tenant_id, created_at) VALUES('t1', 0);"
            "INSERT INTO units(unit_id, tenant_id, created_at) VALUES('u1','t1',0);",
            nullptr, nullptr, nullptr), SQLITE_OK);
        SeedSoftDeleted("fh1", 256);

        gc_ = std::make_unique<catalog::gc::GcManager>(catalog_.db(), &sink_, cfg_);

        // Document layer: a real F05 pool + a router whose ListNamespaces yields kNs.
        harness_ = std::make_unique<cortrix::test::NsPoolHarness>(
            std::filesystem::temp_directory_path() /
            ("gcroutes_" + std::to_string(reinterpret_cast<uintptr_t>(this))));
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
        admin_key_ = "gc-admin-key";
        ApiKeyConfig admin_kc;
        admin_kc.key_hash = ApiKeyAuth::HashKey(admin_key_);
        admin_kc.tenant_id = "admin-user";
        admin_kc.permissions = kPermRead | kPermWrite | kPermAdmin;

        user_key_ = "gc-user-key";
        ApiKeyConfig user_kc;
        user_kc.key_hash = ApiKeyAuth::HashKey(user_key_);
        user_kc.tenant_id = "alice";
        user_kc.permissions = kPermRead | kPermWrite;  // no admin
        auth_->LoadKeys({admin_kc, user_kc});

        server_ = std::make_unique<httplib::Server>();
        RegisterGcRoutes(*server_, *gc_, *sweeper_, *auth_);
        port_ = 19400 + (getpid() % 300);
        server_thread_ = std::thread([this] { server_->listen("127.0.0.1", port_); });
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    void TearDown() override {
        if (server_) server_->stop();
        if (server_thread_.joinable()) server_thread_.join();
    }

    void SeedSoftDeleted(const std::string& fh, int64_t size) {
        sqlite3_stmt* stmt = nullptr;
        const char* sql =
            "INSERT INTO file_locations (file_hash, unit_id, ns_id, tenant_id, size_bytes, "
            "blob_uri, ref_count, first_seen_at, status, deleted_at, created_at) "
            "VALUES (?, 'u1','ns1','t1', ?, 'x/y', 0, 0, 'deleted', 1000, 0)";
        ASSERT_EQ(sqlite3_prepare_v2(catalog_.db(), sql, -1, &stmt, nullptr), SQLITE_OK);
        sqlite3_bind_text(stmt, 1, fh.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 2, size);
        ASSERT_EQ(sqlite3_step(stmt), SQLITE_DONE);
        sqlite3_finalize(stmt);
    }

    httplib::Headers Admin() { return {{"Authorization", "Bearer " + admin_key_}}; }
    httplib::Headers AdminConfirm() {
        return {{"Authorization", "Bearer " + admin_key_}, {"X-Ops-Confirm", "true"}};
    }
    httplib::Headers User() { return {{"Authorization", "Bearer " + user_key_}}; }

    // Seed a soft-deleted doc in the pool (for the restore test).
    void SeedSoftDeletedDoc(const std::string& doc_id) {
        cortrix::resource::NamespaceFacade facade(harness_->ipool(), kNs);
        ASSERT_TRUE(facade.Acquire().ok());
        CortrixDoc doc;
        doc.doc_id = doc_id;
        doc.source_type = "upload";
        doc.source_path = doc_id + ".txt";
        doc.status = DocStatus::kReady;
        ASSERT_EQ(facade.store().doc_create(doc), 0);
        ASSERT_EQ(facade.store().doc_soft_delete(doc_id, 1000), 0);
    }

    static constexpr const char* kNs = "sales";
    catalog::gc::NullBlobGcSink sink_;
    catalog::CatalogDb catalog_;
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
    std::string user_key_;
};

TEST_F(GcRoutesTest, StatusReturnsGcStatusShape) {
    // soft_deleted_count comes from the document layer (the sweeper); seed one
    // soft-deleted doc. reclaimable_bytes comes from the catalog layer (256, seeded).
    SeedSoftDeletedDoc("docS");
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/gc/status", Admin());
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 200) << res->body;
    auto body = json::parse(res->body);
    EXPECT_EQ(body["status"], "idle");
    EXPECT_EQ(body["soft_deleted_count"], 1);
    EXPECT_EQ(body["reclaimable_bytes"], 256);
    ASSERT_TRUE(body.contains("last_gc_at"));
    EXPECT_TRUE(body["last_gc_at"].is_null());
}

TEST_F(GcRoutesTest, StatusForbiddenForNonAdmin) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/gc/status", User());
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 403) << res->body;
}

TEST_F(GcRoutesTest, RunRequiresOpsConfirm) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Post("/api/v1/gc/run", Admin(), "", "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400) << res->body;
    auto body = json::parse(res->body);
    EXPECT_EQ(body["error"]["code"], "CX_ERR_OPS_CONFIRM_REQUIRED");
}

TEST_F(GcRoutesTest, RunWithConfirmReturns202GcStatus) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Post("/api/v1/gc/run", AdminConfirm(), "", "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 202) << res->body;
    auto body = json::parse(res->body);
    ASSERT_TRUE(body.contains("status"));
    // After a sweep last_gc_at is populated.
    EXPECT_FALSE(body["last_gc_at"].is_null());
}

TEST_F(GcRoutesTest, PurgeRequiresOpsConfirm) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Post("/api/v1/gc/purge", Admin(), "", "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
}

TEST_F(GcRoutesTest, RestoreReturnsPartialSuccessShape) {
    // Seed a soft-deleted doc in the pool so restore has a target (the route now
    // delegates to the document sweeper, not the catalog GcManager).
    SeedSoftDeletedDoc("docA");

    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Post("/api/v1/gc/restore", Admin(),
                        R"({"document_ids":["docA","ghost"]})", "application/json");
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 200) << res->body;
    auto body = json::parse(res->body);
    ASSERT_TRUE(body.contains("succeeded"));
    ASSERT_TRUE(body.contains("failed"));
    ASSERT_TRUE(body.contains("coverage_ratio"));
    EXPECT_EQ(body["succeeded"].size(), 1u);
    EXPECT_EQ(body["succeeded"][0]["id"], "docA");
    EXPECT_EQ(body["failed"].size(), 1u);
    EXPECT_EQ(body["failed"][0]["id"], "ghost");
    EXPECT_EQ(body["failed"][0]["error_code"], "CX_ERR_GC_DOC_NOT_FOUND");
}

TEST_F(GcRoutesTest, VacuumAndReindexAccepted) {
    httplib::Client cli("127.0.0.1", port_);
    auto v = cli.Post("/api/v1/maintenance/vacuum", Admin(), "", "application/json");
    ASSERT_TRUE(v);
    EXPECT_EQ(v->status, 202) << v->body;

    auto r = cli.Post("/api/v1/maintenance/reindex", Admin(), "{}", "application/json");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 202) << r->body;
}

TEST_F(GcRoutesTest, MaintenanceForbiddenForNonAdmin) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Post("/api/v1/maintenance/vacuum", User(), "", "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 403);
}

}  // namespace
}  // namespace cortrix
