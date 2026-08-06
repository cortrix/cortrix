#include <gtest/gtest.h>

#include <sqlite3.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "httplib.h"
#include <nlohmann/json.hpp>

#include "cortrix/auth/api_key_auth.h"
#include "cortrix/auth/auth_context.h"
#include "cortrix/catalog/schema_provider.h"  // SchemaMigrator
#include "cortrix/common/in_memory_global_config.h"
#include "cortrix/observability/operation_log_schema.h"
#include "cortrix/observability/operation_logger_impl.h"
#include "cortrix/server/routes/operations_routes.h"

// Operation log S3 coverage: GET /api/v1/operations route. Exercises the 8 query
// params, the admin cross-user permission gate (permission grading), pagination +
// sort, and the 6 CX_ERR_OPLOG_* Agent-friendly error bodies over a real
// httplib server backed by an in-memory operation_log (migrated with the operation log
// provider) and a real ApiKeyAuth (MVP: user_id == tenant_id).
namespace cortrix {
namespace {

using json = nlohmann::json;
using observability::OperationLogEntry;

class OperationsRoutesTest : public ::testing::Test {
protected:
    void SetUp() override {
        // In-memory global db + operation log schema (operation_log + indices).
        ASSERT_EQ(sqlite3_open(":memory:", &db_), SQLITE_OK);
        sqlite3_exec(db_, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
        cortrix::catalog::SchemaMigrator m;
        m.Register(&provider_);
        ASSERT_TRUE(m.MigrateCatalog(db_).ok());

        config_ = std::make_shared<InMemoryGlobalConfig>();
        logger_ = std::make_shared<observability::OperationLogger>(db_, config_);

        // Two keys: an admin (user_id "admin-user") and a normal user "alice".
        auth_ = std::make_unique<ApiKeyAuth>();
        admin_key_ = "ops-admin-key";
        ApiKeyConfig admin_kc;
        admin_kc.key_hash = ApiKeyAuth::HashKey(admin_key_);
        admin_kc.tenant_id = "admin-user";  // -> AuthContext.user_id
        admin_kc.permissions = kPermRead | kPermAdmin;

        alice_key_ = "ops-alice-key";
        ApiKeyConfig alice_kc;
        alice_kc.key_hash = ApiKeyAuth::HashKey(alice_key_);
        alice_kc.tenant_id = "alice";
        alice_kc.permissions = kPermRead;

        auth_->LoadKeys({admin_kc, alice_kc});

        // Seed rows: alice x3 (two memory, one query), bob x2.
        Seed("alice", "memory_create", "memory", "sales", "blk-1", "likes coffee", 1000);
        Seed("alice", "memory_delete", "memory", "sales", "blk-1", "forgot coffee", 2000);
        Seed("alice", "query", "query", "sales", "ix-9", "what coffee", 3000);
        Seed("bob",   "upload", "document", "eng", "doc-7", "spec.pdf", 1500);
        Seed("bob",   "ns_create", "namespace", "eng", "eng", "made eng", 2500);

        server_ = std::make_unique<httplib::Server>();
        RegisterOperationsRoutes(*server_, *logger_, *auth_);
        port_ = 19800 + (getpid() % 400);
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

// ---- basic list + self-scope --------------------------------------------------

// Alice with no user_id param sees only her own 3 rows (default scope).
TEST_F(OperationsRoutesTest, ListSelfScopeReturnsOwnRows) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/operations", Alice());
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 200) << res->body;

    auto body = json::parse(res->body);
    ASSERT_TRUE(body.contains("operations"));
    EXPECT_EQ(body["total_count"], 3);
    EXPECT_EQ(body["operations"].size(), 3u);
    for (const auto& op : body["operations"]) {
        EXPECT_EQ(op["user_id"], "alice");
    }
    // top-level shape (Lead-decided) + meta paging hints.
    EXPECT_TRUE(body.contains("limit"));
    EXPECT_TRUE(body.contains("offset"));
    ASSERT_TRUE(body.contains("meta"));
    EXPECT_TRUE(body["meta"].contains("has_next"));
    EXPECT_TRUE(body["meta"].contains("next_offset"));
}

// A row's optional columns round-trip; sort defaults to DESC (newest first).
TEST_F(OperationsRoutesTest, ListDefaultSortDescAndRowShape) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/operations", Alice());
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 200);
    auto body = json::parse(res->body);
    const auto& ops = body["operations"];
    ASSERT_EQ(ops.size(), 3u);
    EXPECT_EQ(ops[0]["timestamp"], 3000);  // newest first (DESC default)
    EXPECT_EQ(ops[2]["timestamp"], 1000);
    // Row shape per
    const auto& top = ops[0];
    for (const char* k : {"id", "timestamp", "user_id", "action", "namespace_id",
                          "resource_type", "resource_id", "summary", "trace_id",
                          "session_id"}) {
        EXPECT_TRUE(top.contains(k)) << k;
    }
    EXPECT_TRUE(top["trace_id"].is_null());  // not seeded -> JSON null
}

// ---- filters ------------------------------------------------------------------

TEST_F(OperationsRoutesTest, FilterByAction) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/operations?action=memory_create", Alice());
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 200);
    auto body = json::parse(res->body);
    EXPECT_EQ(body["total_count"], 1);
    EXPECT_EQ(body["operations"][0]["action"], "memory_create");
}

TEST_F(OperationsRoutesTest, FilterByActionInCsvMultiSelect) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/operations?action_in=memory_create,memory_delete", Alice());
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 200);
    auto body = json::parse(res->body);
    EXPECT_EQ(body["total_count"], 2);
}

TEST_F(OperationsRoutesTest, FilterByResourceTypeAndTimestampRange) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get(
        "/api/v1/operations?resource_type=memory&from_timestamp=1500&to_timestamp=3500",
        Alice());
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 200);
    auto body = json::parse(res->body);
    // memory rows are ts 1000 + 2000; range [1500,3500] keeps only 2000.
    EXPECT_EQ(body["total_count"], 1);
    EXPECT_EQ(body["operations"][0]["timestamp"], 2000);
}

// ---- pagination + sort_order --------------------------------------------------

TEST_F(OperationsRoutesTest, PaginationAndAscSort) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/operations?limit=2&offset=0&sort_order=asc", Alice());
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 200);
    auto body = json::parse(res->body);
    EXPECT_EQ(body["total_count"], 3);
    ASSERT_EQ(body["operations"].size(), 2u);
    EXPECT_EQ(body["operations"][0]["timestamp"], 1000);  // ASC -> oldest first
    EXPECT_EQ(body["meta"]["has_next"], true);
    EXPECT_EQ(body["meta"]["next_offset"], 2);
    EXPECT_EQ(body["limit"], 2);
}

// ---- admin cross-user (permission grading) -------------------------------

TEST_F(OperationsRoutesTest, AdminCrossUserFilterReturnsTargetRows) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/operations?user_id=bob", Admin());
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 200) << res->body;
    auto body = json::parse(res->body);
    EXPECT_EQ(body["total_count"], 2);
    for (const auto& op : body["operations"]) EXPECT_EQ(op["user_id"], "bob");
}

TEST_F(OperationsRoutesTest, NonAdminCrossUserIsUnauthorized) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/operations?user_id=bob", Alice());
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 403);
    auto body = json::parse(res->body);
    ASSERT_TRUE(body.contains("error"));
    EXPECT_EQ(body["error"]["code"], "CX_ERR_OPLOG_UNAUTHORIZED");
    EXPECT_EQ(body["error"]["category"], "auth");
    EXPECT_EQ(body["error"]["retryable"], false);
    ASSERT_TRUE(body["error"].contains("structured_data"));
    EXPECT_EQ(body["error"]["structured_data"]["required_role"], "admin");
}

// A non-admin asking for their OWN rows via explicit user_id is allowed.
TEST_F(OperationsRoutesTest, NonAdminOwnUserIdAllowed) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/operations?user_id=alice", Alice());
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200) << res->body;
    auto body = json::parse(res->body);
    EXPECT_EQ(body["total_count"], 3);
}

// ---- CX_ERR_OPLOG_* error bodies ---------------------------------------

TEST_F(OperationsRoutesTest, BadSortOrderIsInvalidFilter) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/operations?sort_order=sideways", Alice());
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
    auto body = json::parse(res->body);
    EXPECT_EQ(body["error"]["code"], "CX_ERR_OPLOG_INVALID_FILTER");
    EXPECT_EQ(body["error"]["category"], "permanent");
    ASSERT_TRUE(body["error"]["structured_data"].contains("invalid_field"));
    ASSERT_TRUE(body["error"]["structured_data"].contains("reason"));
}

TEST_F(OperationsRoutesTest, LimitOverCapIsInvalidFilter) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/operations?limit=500", Alice());  // > 200 cap
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
    auto body = json::parse(res->body);
    EXPECT_EQ(body["error"]["code"], "CX_ERR_OPLOG_INVALID_FILTER");
}

TEST_F(OperationsRoutesTest, NonIntegerTimestampIsInvalidFilter) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/operations?from_timestamp=notanumber", Alice());
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
    auto body = json::parse(res->body);
    EXPECT_EQ(body["error"]["code"], "CX_ERR_OPLOG_INVALID_FILTER");
    EXPECT_EQ(body["error"]["structured_data"]["invalid_field"], "from_timestamp");
}

TEST_F(OperationsRoutesTest, InvertedTimestampRangeIsRangeError) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/operations?from_timestamp=5000&to_timestamp=1000", Alice());
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
    auto body = json::parse(res->body);
    EXPECT_EQ(body["error"]["code"], "CX_ERR_OPLOG_INVALID_TIMESTAMP_RANGE");
    EXPECT_EQ(body["error"]["structured_data"]["from"], 5000);
    EXPECT_EQ(body["error"]["structured_data"]["to"], 1000);
}

TEST_F(OperationsRoutesTest, OffsetPastEndIsPaginationOutOfRange) {
    httplib::Client cli("127.0.0.1", port_);
    // alice has 3 rows; offset 50 is past the end.
    auto res = cli.Get("/api/v1/operations?offset=50", Alice());
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
    auto body = json::parse(res->body);
    EXPECT_EQ(body["error"]["code"], "CX_ERR_OPLOG_PAGINATION_OUT_OF_RANGE");
    EXPECT_EQ(body["error"]["structured_data"]["offset"], 50);
    EXPECT_EQ(body["error"]["structured_data"]["total_count"], 3);
}

// ---- auth gate ----------------------------------------------------------------

TEST_F(OperationsRoutesTest, NoAuthIsUnauthenticated) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/operations");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 401);
}

}  // namespace
}  // namespace cortrix
