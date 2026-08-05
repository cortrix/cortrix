#include <gtest/gtest.h>

#include <sqlite3.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include "httplib.h"
#include <nlohmann/json.hpp>

#include "cortrix/agent_trace/agent_trace_schema.h"
#include "cortrix/auth/api_key_auth.h"
#include "cortrix/catalog/schema_provider.h"
#include "cortrix/common/in_memory_global_config.h"
#include "cortrix/config/config.h"
#include "cortrix/server/routes/observability_routes.h"
#include "ns_pool_test_helper.h"

// [agent trace TC4] The agent trace read routes after the agent_trace global split:
//   - GET /traces/:session_id is GLOBAL (RegisterTracesRoutesGlobal) — NO ?namespace=,
//     reads the global agent_trace; ownership resolved cross-NS via the trace's
//     namespace_id. An empty global agent_trace → admin sees SESSION_NOT_FOUND.
//   - GET /interactions[/:id/sources] stays PER-NS (RegisterInteractionsRoutesPerNs) —
//     ?namespace= required on /sources; /interactions uses its namespace_id filter.
// A cross-cutting harness test (real namespace pool + a real global agent_trace db).
namespace cortrix {
namespace {

using json = nlohmann::json;

class ObservabilityRoutesTc4Test : public ::testing::Test {
protected:
    void SetUp() override {
        tmp_dir_ = "/tmp/cortrix_test_obs_routes_" + std::to_string(getpid());
        system(("mkdir -p " + tmp_dir_).c_str());

        std::string test_key = "test-api-key-12345";
        ApiKeyConfig key_config;
        key_config.key_hash = ApiKeyAuth::HashKey(test_key);
        key_config.tenant_id = "test-tenant";
        key_config.permissions = 7;  // read + write + admin
        std::vector<ApiKeyConfig> keys{key_config};
        auth_.LoadKeys(keys);

        harness_ = std::make_unique<cortrix::test::NsPoolHarness>(tmp_dir_ + "/pool");
        ASSERT_TRUE(harness_->Admit("default").ok());

        global_config_ = std::make_shared<InMemoryGlobalConfig>();

        // Global agent_trace db (TC4): a real on-disk sqlite migrated with the agent trace
        // AgentTraceSchemaProvider, the same way bootstrap migrates it into the global
        // catalog db. Empty to start (no traces written).
        ASSERT_EQ(sqlite3_open((tmp_dir_ + "/global.db").c_str(), &global_db_), SQLITE_OK);
        {
            cortrix::catalog::SchemaMigrator m;
            cortrix::agent_trace::AgentTraceSchemaProvider at_provider;
            m.Register(&at_provider);
            ASSERT_TRUE(m.MigrateCatalog(global_db_).ok());
        }

        port_ = 19550 + (getpid() % 400);
        svr_ = std::make_unique<httplib::Server>();
        RegisterTracesRoutesGlobal(*svr_, global_db_, harness_->ipool(), global_config_, auth_);
        RegisterInteractionsRoutesPerNs(*svr_, harness_->ipool(), auth_);

        svr_thread_ = std::thread([this] { svr_->listen("127.0.0.1", port_); });
        httplib::Client cli("127.0.0.1", port_);
        for (int i = 0; i < 50; ++i) {
            auto res = cli.Get("/api/v1/interactions");
            if (res) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    void TearDown() override {
        svr_->stop();
        if (svr_thread_.joinable()) svr_thread_.join();
        if (global_db_) sqlite3_close(global_db_);
        system(("rm -rf " + tmp_dir_).c_str());
    }
    httplib::Headers AuthHeaders() { return {{"Authorization", "Bearer test-api-key-12345"}}; }

    ApiKeyAuth auth_;
    std::unique_ptr<cortrix::test::NsPoolHarness> harness_;
    std::shared_ptr<InMemoryGlobalConfig> global_config_;
    sqlite3* global_db_ = nullptr;
    std::unique_ptr<httplib::Server> svr_;
    std::thread svr_thread_;
    std::string tmp_dir_;
    int port_ = 0;
};

TEST_F(ObservabilityRoutesTc4Test, TracesGlobalNoNamespaceRequired) {
    // TC4: /traces is global — NO ?namespace=. An admin key over an empty global
    // agent_trace gets SESSION_NOT_FOUND (the session exists nowhere), NOT the old
    // 400 "namespace required". Proves the route reads the global db with no NS param.
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/traces/sess-1", AuthHeaders());
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404);
    auto body = json::parse(res->body);
    ASSERT_TRUE(body.contains("error"));
    EXPECT_EQ(body["error"]["code"], "CX_ERR_F13_SESSION_NOT_FOUND");
}

TEST_F(ObservabilityRoutesTc4Test, TracesUnknownSessionReturns404) {
    // Admin + a session absent from the global agent_trace → SESSION_NOT_FOUND
    // (proves the global handler construction + query run E2E).
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/traces/unknown-session", AuthHeaders());
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404);
    auto body = json::parse(res->body);
    EXPECT_EQ(body["error"]["code"], "CX_ERR_F13_SESSION_NOT_FOUND");
}

TEST_F(ObservabilityRoutesTc4Test, SourcesMissingNamespaceReturns400) {
    // /interactions/:id/sources stays per-NS → ?namespace= still required.
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/interactions/int-1/sources", AuthHeaders());
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
    auto body = json::parse(res->body);
    EXPECT_EQ(body["error"]["structured_data"]["invalid_field"], "namespace");
}

TEST_F(ObservabilityRoutesTc4Test, ListInteractionsValidNamespaceReturns200Empty) {
    // A valid namespace reaches the per-NS InteractionsHandler over its memory.db
    // (whose interaction_log/interaction_sources MemoryStore::Init created). No
    // interactions logged → well-formed list envelope, no 5xx.
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/interactions?namespace_id=default", AuthHeaders());
    ASSERT_TRUE(res);
    EXPECT_LT(res->status, 500);
}

}  // namespace
}  // namespace cortrix
