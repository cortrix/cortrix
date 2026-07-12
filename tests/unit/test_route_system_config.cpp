// system_config_routes.cpp coverage — the F48 §6.3 agent_llm config API:
//   GET /api/v1/system/agent_llm_config  (read, api_key masked)
//   PUT /api/v1/system/agent_llm_config  (admin only, field-merge semantics)
//
// In-process loopback harness (httplib::Server + RegisterSystemConfigRoutes), the
// same pattern as test_errpath_tenant.cpp. Backed by a real InMemoryGlobalConfig
// (api_key encrypted at rest by SetAgentLlmConfig, decrypted by Get, masked on the
// read shape). Suite name SystemConfigRoute is globally unique. No LLM, no network
// beyond 127.0.0.1. ApiKeyAuth is non-copyable → built in place, passed by ref.
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include "httplib.h"
#include <nlohmann/json.hpp>

#include "cortrix/auth/api_key_auth.h"
#include "cortrix/common/agent_llm_config.h"
#include "cortrix/common/in_memory_global_config.h"
#include "cortrix/server/routes/system_config_routes.h"

namespace cortrix {
namespace {

using json = nlohmann::json;

class SystemConfigRoute : public ::testing::Test {
protected:
    void SetUp() override {
        config_ = std::make_unique<InMemoryGlobalConfig>();
        // Seed an initial provider+key so GET masks something and merge tests have
        // a baseline to preserve.
        AgentLlmConfig seed;
        seed.provider = "openai";
        seed.api_key = "sk-secret-original-key-12345";
        seed.model = "gpt-4o";
        seed.base_url = "https://api.openai.com";
        seed.max_tokens = 1024;
        seed.temperature = 0.3;
        config_->SetAgentLlmConfig(seed);

        auth_ = std::make_unique<ApiKeyAuth>();
        admin_key_ = "syscfg-admin-key";
        user_key_ = "syscfg-readonly-key";
        ApiKeyConfig admin_kc;
        admin_kc.key_hash = ApiKeyAuth::HashKey(admin_key_);
        admin_kc.tenant_id = "admin-user";
        admin_kc.permissions = kPermRead | kPermWrite | kPermAdmin;
        ApiKeyConfig user_kc;
        user_kc.key_hash = ApiKeyAuth::HashKey(user_key_);
        user_kc.tenant_id = "alice";
        user_kc.permissions = kPermRead | kPermWrite;  // write but NOT admin
        auth_->LoadKeys({admin_kc, user_kc});

        static std::atomic<unsigned> ctr{0};
        port_ = 19850 + ((getpid() + ctr.fetch_add(1)) % 200);
        server_ = std::make_unique<httplib::Server>();
        RegisterSystemConfigRoutes(*server_, *config_, *auth_);
        server_thread_ = std::thread([this] { server_->listen("127.0.0.1", port_); });

        httplib::Client cli("127.0.0.1", port_);
        for (int i = 0; i < 50; ++i) {
            auto res = cli.Get("/api/v1/system/agent_llm_config", Admin());
            if (res) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    void TearDown() override {
        if (server_) server_->stop();
        if (server_thread_.joinable()) server_thread_.join();
    }

    httplib::Headers Admin() { return {{"Authorization", "Bearer " + admin_key_}}; }
    httplib::Headers User() { return {{"Authorization", "Bearer " + user_key_}}; }

    std::unique_ptr<InMemoryGlobalConfig> config_;
    std::unique_ptr<ApiKeyAuth> auth_;
    std::unique_ptr<httplib::Server> server_;
    std::thread server_thread_;
    int port_ = 0;
    std::string admin_key_, user_key_;
};

// --- GET masks the api_key (never the plaintext) ------------------------------
TEST_F(SystemConfigRoute, GetMasksApiKey) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/system/agent_llm_config", User());  // read perm OK
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 200);
    auto body = json::parse(res->body);
    EXPECT_EQ(body["provider"], "openai");
    EXPECT_EQ(body["model"], "gpt-4o");
    ASSERT_TRUE(body["api_key"].is_string());
    // The plaintext key must NOT appear; a masked form is returned instead.
    EXPECT_EQ(body["api_key"].get<std::string>().find("sk-secret-original-key-12345"),
              std::string::npos);
    EXPECT_FLOAT_EQ(body["temperature"].get<double>(), 0.3);
    EXPECT_EQ(body["max_tokens"], 1024);
}

// --- PUT with a non-admin (write but not admin) key → 403 admin-required --------
TEST_F(SystemConfigRoute, PutNonAdminForbidden) {
    httplib::Client cli("127.0.0.1", port_);
    json b = {{"model", "gpt-4o-mini"}};
    auto res = cli.Put("/api/v1/system/agent_llm_config", User(), b.dump(),
                       "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 403) << res->body;
    EXPECT_EQ(json::parse(res->body)["error"]["code"], "CX_ERR_AUTH_ADMIN_REQUIRED");
}

// --- PUT with malformed JSON → 400 invalid-request -----------------------------
TEST_F(SystemConfigRoute, PutMalformedJson400) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Put("/api/v1/system/agent_llm_config", Admin(), "{not json",
                       "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400) << res->body;
    EXPECT_EQ(json::parse(res->body)["error"]["code"], "CX_ERR_INVALID_REQUEST");
}

// --- PUT with a non-object JSON body (array) → 400 -----------------------------
TEST_F(SystemConfigRoute, PutNonObjectBody400) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Put("/api/v1/system/agent_llm_config", Admin(), "[1,2,3]",
                       "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400) << res->body;
    EXPECT_EQ(json::parse(res->body)["error"]["code"], "CX_ERR_INVALID_REQUEST");
}

// --- PUT with an unsupported provider → 400 (field-level validation) -----------
TEST_F(SystemConfigRoute, PutUnsupportedProvider400) {
    httplib::Client cli("127.0.0.1", port_);
    json b = {{"provider", "definitely-not-a-provider"}};
    auto res = cli.Put("/api/v1/system/agent_llm_config", Admin(), b.dump(),
                       "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400) << res->body;
    auto err = json::parse(res->body)["error"];
    EXPECT_EQ(err["code"], "CX_ERR_INVALID_REQUEST");
}

// --- PUT merge: omitted fields keep their current value ------------------------
TEST_F(SystemConfigRoute, PutPartialMergeKeepsOmittedFields) {
    httplib::Client cli("127.0.0.1", port_);
    // Only change model + temperature; provider/base_url/max_tokens must persist.
    json b = {{"model", "gpt-4o-mini"}, {"temperature", 0.9}};
    auto res = cli.Put("/api/v1/system/agent_llm_config", Admin(), b.dump(),
                       "application/json");
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 200) << res->body;
    auto body = json::parse(res->body);
    EXPECT_EQ(body["model"], "gpt-4o-mini");          // updated
    EXPECT_FLOAT_EQ(body["temperature"].get<double>(), 0.9);  // updated
    EXPECT_EQ(body["provider"], "openai");            // preserved
    EXPECT_EQ(body["base_url"], "https://api.openai.com");  // preserved
    EXPECT_EQ(body["max_tokens"], 1024);              // preserved
}

// --- PUT api_key="" leaves the stored key unchanged (never cleared) -------------
TEST_F(SystemConfigRoute, PutEmptyApiKeyKeepsCurrent) {
    httplib::Client cli("127.0.0.1", port_);
    json b = {{"api_key", ""}, {"provider", "claude"}};
    auto res = cli.Put("/api/v1/system/agent_llm_config", Admin(), b.dump(),
                       "application/json");
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 200) << res->body;
    auto body = json::parse(res->body);
    EXPECT_EQ(body["provider"], "claude");
    // The stored key was not cleared: api_key still masks a non-null value.
    ASSERT_TRUE(body["api_key"].is_string());
    EXPECT_FALSE(body["api_key"].get<std::string>().empty());
    // Re-read confirms the key survived (a cleared key would surface as null).
    auto reread = json::parse(cli.Get("/api/v1/system/agent_llm_config", Admin())->body);
    EXPECT_FALSE(reread["api_key"].is_null());
}

// --- PUT a fresh valid api_key + all fields → 200 round-trip --------------------
TEST_F(SystemConfigRoute, PutFullValidUpdate) {
    httplib::Client cli("127.0.0.1", port_);
    json b = {{"provider", "deepseek"},
              {"api_key", "sk-new-rotated-key"},
              {"model", "deepseek-chat"},
              {"base_url", "https://api.deepseek.com"},
              {"max_tokens", 2048},
              {"temperature", 0.5}};
    auto res = cli.Put("/api/v1/system/agent_llm_config", Admin(), b.dump(),
                       "application/json");
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 200) << res->body;
    auto body = json::parse(res->body);
    EXPECT_EQ(body["provider"], "deepseek");
    EXPECT_EQ(body["model"], "deepseek-chat");
    EXPECT_EQ(body["max_tokens"], 2048);
    EXPECT_FLOAT_EQ(body["temperature"].get<double>(), 0.5);
    // The new key is masked, never echoed in plaintext.
    EXPECT_EQ(body["api_key"].get<std::string>().find("sk-new-rotated-key"),
              std::string::npos);
}

// --- PUT max_tokens <= 0 rejected with a field-level error (FYI-4) --------------
TEST_F(SystemConfigRoute, PutNonPositiveMaxTokensRejected400) {
    httplib::Client cli("127.0.0.1", port_);
    for (int bad : {0, -128}) {
        json b = {{"max_tokens", bad}};
        auto res = cli.Put("/api/v1/system/agent_llm_config", Admin(), b.dump(),
                           "application/json");
        ASSERT_TRUE(res);
        EXPECT_EQ(res->status, 400) << res->body;
        EXPECT_EQ(json::parse(res->body)["error"]["code"], "CX_ERR_INVALID_REQUEST");
    }
    // Both rejected writes left the stored value untouched.
    auto body = json::parse(cli.Get("/api/v1/system/agent_llm_config", Admin())->body);
    EXPECT_EQ(body["max_tokens"], 1024);
}

// --- PUT oversize max_tokens clamped to the cap; echo = effective value (FYI-4) -
TEST_F(SystemConfigRoute, PutOversizeMaxTokensClampedToCap) {
    httplib::Client cli("127.0.0.1", port_);
    json b = {{"max_tokens", 1000000000}};
    auto res = cli.Put("/api/v1/system/agent_llm_config", Admin(), b.dump(),
                       "application/json");
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 200) << res->body;
    EXPECT_EQ(json::parse(res->body)["max_tokens"], kAgentLlmMaxTokensCap);
    // Persisted, not just echoed.
    auto reread = json::parse(cli.Get("/api/v1/system/agent_llm_config", Admin())->body);
    EXPECT_EQ(reread["max_tokens"], kAgentLlmMaxTokensCap);
}

}  // namespace
}  // namespace cortrix
