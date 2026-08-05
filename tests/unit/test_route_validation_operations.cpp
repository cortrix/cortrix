// Route request-validation matrices for the operation-log query route
// (GET /api/v1/operations). BREADTH coverage of bad query params (non-integer
// limit/offset/timestamp, out-of-range limit, inverted ranges, bad enums),
// the admin cross-user permission gate, missing auth, and method-not-allowed,
// asserting the CX_ERR_OPLOG_* GEN-Agent envelope.
//
// Uses the SAME in-process harness as test_operations_routes.cpp (real
// OperationLogger over an in-memory operation_log + real ApiKeyAuth). No live
// network / LLM.
//
// Suite/fixture names area-prefixed (OpsRouteValMatrix*) for global uniqueness.
#include <gtest/gtest.h>

#include <sqlite3.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include "httplib.h"
#include <nlohmann/json.hpp>

#include "cortrix/auth/api_key_auth.h"
#include "cortrix/auth/auth_context.h"
#include "cortrix/catalog/schema_provider.h"
#include "cortrix/common/in_memory_global_config.h"
#include "cortrix/observability/operation_log_schema.h"
#include "cortrix/observability/operation_logger_impl.h"
#include "cortrix/server/routes/operations_routes.h"

namespace cortrix {
namespace {

using json = nlohmann::json;
using observability::OperationLogEntry;

class OpsRouteValMatrix : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_EQ(sqlite3_open(":memory:", &db_), SQLITE_OK);
        sqlite3_exec(db_, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
        cortrix::catalog::SchemaMigrator m;
        m.Register(&provider_);
        ASSERT_TRUE(m.MigrateCatalog(db_).ok());

        config_ = std::make_shared<InMemoryGlobalConfig>();
        logger_ = std::make_shared<observability::OperationLogger>(db_, config_);

        auth_ = std::make_unique<ApiKeyAuth>();
        admin_key_ = "orvm-admin-key";
        ApiKeyConfig admin_kc;
        admin_kc.key_hash = ApiKeyAuth::HashKey(admin_key_);
        admin_kc.tenant_id = "admin-user";
        admin_kc.permissions = kPermRead | kPermAdmin;

        alice_key_ = "orvm-alice-key";
        ApiKeyConfig alice_kc;
        alice_kc.key_hash = ApiKeyAuth::HashKey(alice_key_);
        alice_kc.tenant_id = "alice";
        alice_kc.permissions = kPermRead;
        auth_->LoadKeys({admin_kc, alice_kc});

        // 3 rows for alice so pagination/offset boundaries are meaningful.
        Seed("alice", "memory_create", "memory", "sales", "b1", "s1", 1000);
        Seed("alice", "memory_delete", "memory", "sales", "b1", "s2", 2000);
        Seed("alice", "query", "query", "sales", "ix", "s3", 3000);
        Seed("bob", "upload", "document", "eng", "d7", "spec", 1500);

        server_ = std::make_unique<httplib::Server>();
        RegisterOperationsRoutes(*server_, *logger_, *auth_);
        port_ = 18450 + (getpid() % 300);
        server_thread_ = std::thread([this]() { server_->listen("127.0.0.1", port_); });
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    void TearDown() override {
        if (server_) server_->stop();
        if (server_thread_.joinable()) server_thread_.join();
        logger_.reset();
        if (db_) sqlite3_close(db_);
    }

    void Seed(const std::string& user, const std::string& action,
              const std::string& rtype, const std::string& ns,
              const std::string& rid, const std::string& summary, int64_t ts) {
        OperationLogEntry e;
        e.timestamp = ts;
        e.user_id = user;
        e.action = action;
        e.resource_type = rtype;
        e.namespace_id = ns;
        e.resource_id = rid;
        e.summary = summary;
        logger_->Log(e);
    }

    httplib::Headers Admin() { return {{"Authorization", "Bearer " + admin_key_}}; }
    httplib::Headers Alice() { return {{"Authorization", "Bearer " + alice_key_}}; }

    observability::OperationLogSchemaProvider provider_;
    sqlite3* db_ = nullptr;
    std::shared_ptr<InMemoryGlobalConfig> config_;
    std::shared_ptr<observability::OperationLogger> logger_;
    std::unique_ptr<ApiKeyAuth> auth_;
    std::unique_ptr<httplib::Server> server_;
    std::thread server_thread_;
    int port_ = 0;
    std::string admin_key_;
    std::string alice_key_;
};

// ---- auth matrix ----------------------------------------------------------

TEST_F(OpsRouteValMatrix, NoAuth401) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/operations");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 401);
}

TEST_F(OpsRouteValMatrix, UnknownKey401) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/operations", {{"Authorization", "Bearer ghost-key"}});
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 401);
}

TEST_F(OpsRouteValMatrix, BadScheme401) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/operations", {{"Authorization", "Basic " + alice_key_}});
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 401);
}

// ---- cross-user permission gate (403) -------------------------------------

TEST_F(OpsRouteValMatrix, NonAdminCrossUser403) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/operations?user_id=bob", Alice());
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 403);
    auto body = json::parse(res->body);
    EXPECT_EQ(body["error"]["code"], "CX_ERR_OPLOG_UNAUTHORIZED");
    EXPECT_EQ(body["error"]["category"], "auth");
    EXPECT_EQ(body["error"]["retryable"], false);
    EXPECT_EQ(body["error"]["structured_data"]["required_role"], "admin");
}

TEST_F(OpsRouteValMatrix, NonAdminOwnUserIdAllowed200) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/operations?user_id=alice", Alice());
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);
}

TEST_F(OpsRouteValMatrix, AdminCrossUserAllowed200) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/operations?user_id=bob", Admin());
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);
}

// ---- bad sort_order enum matrix (TEST_P) ----------------------------------

class OpsRouteValBadSort
    : public OpsRouteValMatrix,
      public ::testing::WithParamInterface<std::string> {};

TEST_P(OpsRouteValBadSort, InvalidFilter400) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/operations?sort_order=" + GetParam(), Alice());
    ASSERT_TRUE(res) << GetParam();
    EXPECT_EQ(res->status, 400) << GetParam();
    auto body = json::parse(res->body);
    EXPECT_EQ(body["error"]["code"], "CX_ERR_OPLOG_INVALID_FILTER") << GetParam();
}

// NOTE: the route uppercases sort_order before validating, so only tokens that
// are invalid even after upper-casing (i.e. != "ASC" and != "DESC") belong here.
INSTANTIATE_TEST_SUITE_P(
    Values, OpsRouteValBadSort,
    ::testing::Values(std::string("sideways"), std::string("ascending"),
                      std::string("up"), std::string("descending2"),
                      std::string("1"), std::string("rand"),
                      std::string("ascdesc"), std::string("descend"),
                      std::string("ascend"), std::string("desc_order"),
                      std::string("zzz"), std::string("none")));

// ---- non-integer numeric params matrix (TEST_P) ---------------------------
// Each pair is (param_name, bad_value); all should be CX_ERR_OPLOG_INVALID_FILTER.

struct OpsBadNumeric {
    std::string param;
    std::string value;
};

class OpsRouteValBadNumeric
    : public OpsRouteValMatrix,
      public ::testing::WithParamInterface<OpsBadNumeric> {};

TEST_P(OpsRouteValBadNumeric, InvalidFilter400) {
    const auto& tc = GetParam();
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/operations?" + tc.param + "=" + tc.value, Alice());
    ASSERT_TRUE(res) << tc.param << "=" << tc.value;
    EXPECT_EQ(res->status, 400) << tc.param << "=" << tc.value << " body=" << res->body;
    auto body = json::parse(res->body);
    EXPECT_EQ(body["error"]["code"], "CX_ERR_OPLOG_INVALID_FILTER")
        << tc.param << "=" << tc.value;
}

INSTANTIATE_TEST_SUITE_P(
    Params, OpsRouteValBadNumeric,
    ::testing::Values(
        OpsBadNumeric{"limit", "notanumber"},
        OpsBadNumeric{"limit", "1.5"},
        OpsBadNumeric{"limit", "abc"},
        OpsBadNumeric{"limit", "12x"},
        OpsBadNumeric{"limit", "0x10"},
        OpsBadNumeric{"limit", "--5"},
        OpsBadNumeric{"offset", "notanumber"},
        OpsBadNumeric{"offset", "x"},
        OpsBadNumeric{"offset", "3.14"},
        OpsBadNumeric{"offset", "1e3"},
        OpsBadNumeric{"offset", "0xFF"},
        OpsBadNumeric{"from_timestamp", "notanumber"},
        OpsBadNumeric{"from_timestamp", "yesterday"},
        OpsBadNumeric{"from_timestamp", "12.34"},
        OpsBadNumeric{"to_timestamp", "notanumber"},
        OpsBadNumeric{"to_timestamp", "soon"},
        OpsBadNumeric{"to_timestamp", "9e9"}));

// ---- out-of-range limit matrix (TEST_P) -----------------------------------
// limit cap is 200; over-cap values are rejected.

class OpsRouteValLimitOverCap
    : public OpsRouteValMatrix,
      public ::testing::WithParamInterface<int> {};

TEST_P(OpsRouteValLimitOverCap, InvalidFilter400) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/operations?limit=" + std::to_string(GetParam()), Alice());
    ASSERT_TRUE(res) << GetParam();
    EXPECT_EQ(res->status, 400) << GetParam();
    EXPECT_EQ(json::parse(res->body)["error"]["code"], "CX_ERR_OPLOG_INVALID_FILTER");
}

INSTANTIATE_TEST_SUITE_P(Limits, OpsRouteValLimitOverCap,
                         ::testing::Values(201, 250, 300, 500, 1000, 5000, 99999));

// ---- inverted timestamp range ---------------------------------------------

TEST_F(OpsRouteValMatrix, InvertedTimestampRange400) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/operations?from_timestamp=5000&to_timestamp=1000", Alice());
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
    auto body = json::parse(res->body);
    EXPECT_EQ(body["error"]["code"], "CX_ERR_OPLOG_INVALID_TIMESTAMP_RANGE");
    EXPECT_EQ(body["error"]["structured_data"]["from"], 5000);
    EXPECT_EQ(body["error"]["structured_data"]["to"], 1000);
}

// ---- offset-past-end pagination -------------------------------------------

TEST_F(OpsRouteValMatrix, OffsetPastEnd400) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/operations?offset=50", Alice());  // alice has 3
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
    auto body = json::parse(res->body);
    EXPECT_EQ(body["error"]["code"], "CX_ERR_OPLOG_PAGINATION_OUT_OF_RANGE");
    EXPECT_EQ(body["error"]["structured_data"]["offset"], 50);
    EXPECT_EQ(body["error"]["structured_data"]["total_count"], 3);
}

// ---- valid params still pass (negative control matrix) --------------------

class OpsRouteValGoodParam
    : public OpsRouteValMatrix,
      public ::testing::WithParamInterface<std::string> {};

TEST_P(OpsRouteValGoodParam, Returns200) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/operations?" + GetParam(), Alice());
    ASSERT_TRUE(res) << GetParam();
    EXPECT_EQ(res->status, 200) << GetParam() << " body=" << res->body;
}

INSTANTIATE_TEST_SUITE_P(
    Params, OpsRouteValGoodParam,
    ::testing::Values(std::string("limit=1"), std::string("limit=200"),
                      std::string("limit=50"), std::string("offset=0"),
                      std::string("offset=1"), std::string("sort_order=asc"),
                      std::string("sort_order=desc"), std::string("sort_order=ASC"),
                      std::string("sort_order=DESC"), std::string("action=query"),
                      std::string("action_in=query,memory_create"),
                      std::string("resource_type=memory"),
                      std::string("resource_type=query"),
                      std::string("from_timestamp=0&to_timestamp=9999"),
                      std::string("from_timestamp=1000"),
                      std::string("namespace_id=sales")));

// ---- method-not-allowed / unknown route -----------------------------------

TEST_F(OpsRouteValMatrix, PostOnGetRouteGeneric404) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Post("/api/v1/operations", Alice(), "{}", "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404);
}

TEST_F(OpsRouteValMatrix, UnknownRouteGeneric404) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/operations/extra", Alice());
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404);
}

}  // namespace
}  // namespace cortrix
