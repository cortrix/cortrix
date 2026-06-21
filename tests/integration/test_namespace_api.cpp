#include <gtest/gtest.h>
#include <thread>
#include <chrono>

#include "httplib.h"
#include <nlohmann/json.hpp>

#include "cortrix/config/config.h"
#include "cortrix/logging/logging.h"
#include "cortrix/auth/api_key_auth.h"
#include "cortrix/namespace/namespace_manager.h"
#include "cortrix/server/http_server.h"

namespace cortrix {
namespace {

class NamespaceApiTest : public ::testing::Test {
protected:
    void SetUp() override {
        tmp_dir_ = "/tmp/cortrix_test_ns_api_" + std::to_string(getpid());
        system(("mkdir -p " + tmp_dir_).c_str());

        test_key_ = "admin-api-key";
        test_hash_ = ApiKeyAuth::HashKey(test_key_);

        config_.server.host = "127.0.0.1";
        config_.server.thread_count = 2;
        config_.auth.enabled = true;
        config_.ns.data_dir = tmp_dir_;

        ApiKeyConfig kc;
        kc.key_hash = test_hash_;
        kc.tenant_id = "admin-tenant";
        kc.permissions = kPermRead | kPermWrite | kPermAdmin;
        kc.expires_at = 0;
        config_.auth.api_keys.push_back(kc);

        port_ = 18280 + (getpid() % 1000);
        config_.server.port = port_;

        ns_mgr_ = std::make_unique<NamespaceManager>(config_.ns.data_dir);
        ns_mgr_->Init();

        auth_.LoadKeys(config_.auth.api_keys);

        // [V6] The per-key static allow-list was removed; namespace visibility is now
        // a runtime seam. This MVP server stores namespaces in NamespaceManager (the
        // F12 catalog the real PermissionService reads is not wired on this path), so
        // the per-namespace gate stays ungated (admin tenant in single-tenant CE) and
        // the authorized-set lister — consumed by GET /namespaces (filter=true) —
        // resolves to the manager's namespaces, mirroring ListAuthorizedNamespaces.
        auth_.SetNamespaceLister([this](const AuthContext&) {
            std::vector<std::string> names;
            for (const auto& ns : ns_mgr_->List({})) names.push_back(ns.name);
            return names;
        });

        server_ = std::make_unique<CortrixHttpServer>(config_, auth_, *ns_mgr_);
        server_->RegisterRoutes();

        server_thread_ = std::thread([this] { server_->Start(); });

        httplib::Client cli("127.0.0.1", port_);
        for (int i = 0; i < 50; ++i) {
            auto res = cli.Get("/api/v1/health");
            if (res) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    void TearDown() override {
        server_->Stop();
        if (server_thread_.joinable()) {
            server_thread_.join();
        }
        system(("rm -rf " + tmp_dir_).c_str());
    }

    httplib::Headers AuthHeaders() {
        return {{"Authorization", "Bearer " + test_key_}};
    }

    CortrixConfig config_;
    ApiKeyAuth auth_;
    std::unique_ptr<NamespaceManager> ns_mgr_;
    std::unique_ptr<CortrixHttpServer> server_;
    std::thread server_thread_;
    std::string tmp_dir_;
    int port_;
    std::string test_key_;
    std::string test_hash_;
};

TEST_F(NamespaceApiTest, CreateNamespace) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Post("/api/v1/namespaces", AuthHeaders(),
                        R"({"name":"my-ns"})", "application/json");

    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 201);

    auto body = nlohmann::json::parse(res->body);
    EXPECT_EQ(body["name"], "my-ns");
    EXPECT_TRUE(body.contains("created_at"));
    EXPECT_EQ(body["doc_count"], 0);
    EXPECT_EQ(body["block_count"], 0);
}

TEST_F(NamespaceApiTest, CreateDuplicateReturns409) {
    httplib::Client cli("127.0.0.1", port_);
    cli.Post("/api/v1/namespaces", AuthHeaders(),
             R"({"name":"dup-ns"})", "application/json");

    auto res = cli.Post("/api/v1/namespaces", AuthHeaders(),
                        R"({"name":"dup-ns"})", "application/json");

    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 409);
    auto body = nlohmann::json::parse(res->body);
    EXPECT_EQ(body["error"]["code"], "CONFLICT");
}

TEST_F(NamespaceApiTest, CreateInvalidNameReturns400) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Post("/api/v1/namespaces", AuthHeaders(),
                        R"({"name":"invalid name!"})", "application/json");

    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
    auto body = nlohmann::json::parse(res->body);
    EXPECT_EQ(body["error"]["code"], "INVALID_ARGUMENT");
}

TEST_F(NamespaceApiTest, CreateInvalidJsonReturns400) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Post("/api/v1/namespaces", AuthHeaders(),
                        "not json", "application/json");

    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
}

TEST_F(NamespaceApiTest, ListNamespaces) {
    httplib::Client cli("127.0.0.1", port_);
    cli.Post("/api/v1/namespaces", AuthHeaders(),
             R"({"name":"ns-a"})", "application/json");
    cli.Post("/api/v1/namespaces", AuthHeaders(),
             R"({"name":"ns-b"})", "application/json");

    auto res = cli.Get("/api/v1/namespaces", AuthHeaders());

    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);

    auto body = nlohmann::json::parse(res->body);
    EXPECT_EQ(body["total"], 3);  // default + ns-a + ns-b
    EXPECT_TRUE(body["namespaces"].is_array());
}

TEST_F(NamespaceApiTest, GetNamespace) {
    httplib::Client cli("127.0.0.1", port_);
    cli.Post("/api/v1/namespaces", AuthHeaders(),
             R"({"name":"get-ns"})", "application/json");

    auto res = cli.Get("/api/v1/namespaces/get-ns", AuthHeaders());

    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);

    auto body = nlohmann::json::parse(res->body);
    EXPECT_EQ(body["name"], "get-ns");
}

TEST_F(NamespaceApiTest, GetNonexistentReturns404) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/namespaces/doesnt-exist", AuthHeaders());

    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404);

    auto body = nlohmann::json::parse(res->body);
    EXPECT_EQ(body["error"]["code"], "NOT_FOUND");
}

TEST_F(NamespaceApiTest, DeleteNamespace) {
    httplib::Client cli("127.0.0.1", port_);
    cli.Post("/api/v1/namespaces", AuthHeaders(),
             R"({"name":"del-ns"})", "application/json");

    auto res = cli.Delete("/api/v1/namespaces/del-ns", AuthHeaders());

    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 204);

    // Verify it's gone
    res = cli.Get("/api/v1/namespaces/del-ns", AuthHeaders());
    EXPECT_EQ(res->status, 404);
}

TEST_F(NamespaceApiTest, DeleteDefaultReturns400) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Delete("/api/v1/namespaces/default", AuthHeaders());

    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);

    auto body = nlohmann::json::parse(res->body);
    EXPECT_EQ(body["error"]["code"], "INVALID_ARGUMENT");
}

TEST_F(NamespaceApiTest, FullCrudFlow) {
    httplib::Client cli("127.0.0.1", port_);

    // Create
    auto res = cli.Post("/api/v1/namespaces", AuthHeaders(),
                        R"({"name":"crud-ns"})", "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 201);

    // List (should include crud-ns)
    res = cli.Get("/api/v1/namespaces", AuthHeaders());
    ASSERT_TRUE(res);
    auto body = nlohmann::json::parse(res->body);
    bool found = false;
    for (const auto& ns : body["namespaces"]) {
        if (ns["name"] == "crud-ns") found = true;
    }
    EXPECT_TRUE(found);

    // Get
    res = cli.Get("/api/v1/namespaces/crud-ns", AuthHeaders());
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);

    // Delete
    res = cli.Delete("/api/v1/namespaces/crud-ns", AuthHeaders());
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 204);

    // Verify deleted
    res = cli.Get("/api/v1/namespaces/crud-ns", AuthHeaders());
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404);
}

}  // namespace
}  // namespace cortrix
