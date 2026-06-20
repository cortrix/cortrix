// Error-path coverage for the catalog GC codes. Each CX_ERR_* below had ZERO
// referencing test. CX_ERR_GC_INTERNAL / _VACUUM_FAILED / _REINDEX_FAILED are carried
// as a "<CODE>: detail" prefix on the GcManager Status message (CODING_CONVENTIONS §3);
// CX_ERR_GC_INVALID_BODY is a real envelope error.code written by the restore route's
// WriteAgentError on a malformed JSON body.
//
// GcManager borrows a raw sqlite3* it does not own, so these tests hand it a handle
// they fully control (no schema / an open txn / a deregistered collation) to drive the
// failure branches with ordinary inputs — no syscall fault injection needed.

#include <gtest/gtest.h>

#include <sqlite3.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

#include "httplib.h"
#include <nlohmann/json.hpp>

#include "cortrix/auth/api_key_auth.h"
#include "cortrix/catalog/batch_result.h"
#include "cortrix/catalog/catalog_db.h"
#include "cortrix/catalog/gc/blob_gc_sink.h"
#include "cortrix/catalog/gc/document_gc_sweeper.h"
#include "cortrix/catalog/gc/gc_manager.h"
#include "cortrix/config/config.h"
#include "cortrix/server/routes/gc_routes.h"

#include "ns_pool_test_helper.h"

// ===========================================================================
// GcManager direct error paths (CX_ERR_GC_INTERNAL / VACUUM / REINDEX).
// ===========================================================================
namespace cortrix::catalog::gc {
namespace {

// A bare in-memory db with NO catalog schema. GcManager's first prepare ("no such
// table: ...") fails -> CX_ERR_GC_INTERNAL. (Do NOT use CatalogDb::Open, which
// migrates the full schema and would make the prepare succeed.)
TEST(GcManagerErrPathTest, MissingSchemaReturnsGcInternal) {
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    NullBlobGcSink sink;
    GcConfig cfg;
    GcManager mgr(db, &sink, cfg);

    auto s = mgr.GetStatus();  // first prepare hits an absent table
    ASSERT_FALSE(s.ok());
    EXPECT_NE(s.status().message().find("CX_ERR_GC_INTERNAL"), std::string::npos)
        << "message: " << s.status().message();
    sqlite3_close(db);
}

// VACUUM cannot run from within an open transaction -> sqlite3_exec("VACUUM") errors
// -> CX_ERR_GC_VACUUM_FAILED.
TEST(GcManagerErrPathTest, VacuumInsideTransactionReturnsVacuumFailed) {
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(db, "BEGIN;", nullptr, nullptr, nullptr), SQLITE_OK);
    NullBlobGcSink sink;
    GcConfig cfg;
    GcManager mgr(db, &sink, cfg);

    Status s = mgr.Vacuum();
    EXPECT_FALSE(s.ok());
    EXPECT_NE(s.message().find("CX_ERR_GC_VACUUM_FAILED"), std::string::npos)
        << "message: " << s.message();
    sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
    sqlite3_close(db);
}

// An index built on a custom collation that is later deregistered makes REINDEX fail
// with "no such collation sequence" -> CX_ERR_GC_REINDEX_FAILED. (REINDEX, unlike
// VACUUM, succeeds inside a transaction, so the txn trick does not apply here.)
TEST(GcManagerErrPathTest, ReindexWithDeregisteredCollationReturnsReindexFailed) {
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    auto coll = [](void*, int nl, const void* l, int nr, const void* r) -> int {
        int n = nl < nr ? nl : nr;
        int c = std::memcmp(l, r, static_cast<size_t>(n));
        return c != 0 ? c : (nl - nr);
    };
    ASSERT_EQ(sqlite3_create_collation(db, "MYCOLL", SQLITE_UTF8, nullptr, coll), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(db,
        "CREATE TABLE t(x TEXT COLLATE MYCOLL);"
        "CREATE INDEX ix ON t(x);"
        "INSERT INTO t VALUES('a'),('b');",
        nullptr, nullptr, nullptr), SQLITE_OK);
    // Deregister the collation -> any REINDEX touching ix now errors.
    ASSERT_EQ(sqlite3_create_collation(db, "MYCOLL", SQLITE_UTF8, nullptr, nullptr), SQLITE_OK);

    NullBlobGcSink sink;
    GcConfig cfg;
    GcManager mgr(db, &sink, cfg);

    Status s = mgr.Reindex();
    EXPECT_FALSE(s.ok());
    EXPECT_NE(s.message().find("CX_ERR_GC_REINDEX_FAILED"), std::string::npos)
        << "message: " << s.message();
    sqlite3_close(db);
}

}  // namespace
}  // namespace cortrix::catalog::gc

// ===========================================================================
// GC restore route error path (CX_ERR_GC_INVALID_BODY). Mirrors GcRoutesTest.
// ===========================================================================
namespace cortrix {
namespace {

using json = nlohmann::json;
using ::testing::_;
using ::testing::Invoke;

class GcInvalidBodyTest : public ::testing::Test {
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
            ("gcinvalidbody_" + std::to_string(reinterpret_cast<uintptr_t>(this))));
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
        auth_->LoadKeys({admin_kc});

        server_ = std::make_unique<httplib::Server>();
        RegisterGcRoutes(*server_, *gc_, *sweeper_, *auth_);
        port_ = 19700 + (getpid() % 250);
        server_thread_ = std::thread([this] { server_->listen("127.0.0.1", port_); });
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    void TearDown() override {
        if (server_) server_->stop();
        if (server_thread_.joinable()) server_thread_.join();
    }

    httplib::Headers Admin() { return {{"Authorization", "Bearer " + admin_key_}}; }

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
};

// A non-JSON restore body makes nlohmann::json::parse throw -> the route's catch
// emits 400 CX_ERR_GC_INVALID_BODY (gc_routes.cpp:128). Admin auth is required first,
// so a well-formed-but-wrong-shape JSON would NOT trigger it (it parses fine).
TEST_F(GcInvalidBodyTest, MalformedRestoreBodyReturnsInvalidBody) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Post("/api/v1/gc/restore", Admin(), "not json{", "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400) << res->body;
    auto body = json::parse(res->body);
    EXPECT_EQ(body["error"]["code"], "CX_ERR_GC_INVALID_BODY");
}

}  // namespace
}  // namespace cortrix
