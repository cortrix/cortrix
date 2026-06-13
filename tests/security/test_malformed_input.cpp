/// @file test_malformed_input.cpp
/// @brief Security tests: oversized JSON, invalid UTF-8, deep nesting, null bytes
///
/// Validates that HTTP endpoints handle malformed and adversarial input
/// gracefully without crashes, memory corruption, or excessive resource usage.

#include <gtest/gtest.h>
#include <thread>
#include <chrono>
#include <filesystem>

#include "httplib.h"
#include <nlohmann/json.hpp>

#include "cortrix/config/config.h"
#include "cortrix/auth/api_key_auth.h"
#include "cortrix/auth/auth_context.h"
// [wire⑤c follow-up] MVP managers removed; fixture stands on the F05 pool.
#include "unit/ns_pool_test_helper.h"
#include "cortrix/memory/memory_routes.h"
#include "cortrix/query/query_routes.h"
#include "cortrix/query/intent_classifier.h"
#include "cortrix/query/rrf_fusion.h"
#include "cortrix/spc/onnx_embedder.h"
#include "cortrix/spc/spc_manager.h"

namespace cortrix {
namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

class MalformedInputTest : public ::testing::Test {
protected:
    void SetUp() override {
        tmp_dir_ = fs::temp_directory_path() /
                   ("cortrix_sec_malform_" + std::to_string(getpid()));
        fs::create_directories(tmp_dir_);

        config_.ns.data_dir = tmp_dir_.string();
        config_.ns.max_active = 10;
        config_.embedding.dimension = 128;
        config_.memory.inject_recent_turns = 3;
        config_.memory.inject_max_tokens = 2000;
        config_.memory.default_ttl_seconds = 0;
        config_.memory.max_sessions_per_namespace = 100;
        config_.memory.max_interactions_per_session = 500;

        harness_ = std::make_unique<test::NsPoolHarness>(tmp_dir_);
        ASSERT_TRUE(harness_->Admit("default").ok());

        auth_ = std::make_unique<ApiKeyAuth>();
        test_key_ = "sec-malform-test-key";
        ApiKeyConfig kc;
        kc.key_hash = ApiKeyAuth::HashKey(test_key_);
        kc.tenant_id = "malform-tenant";
        kc.permissions = kPermRead | kPermWrite | kPermAdmin;
        auth_->LoadKeys({kc});

        embedder_ = std::make_unique<OnnxEmbedder>("stub.onnx", 128);
        embedder_->Init();
        LlmConfig llm_cfg;
        classifier_ = std::make_unique<IntentClassifier>(llm_cfg);
        fusion_ = std::make_unique<RRFFusion>();
        spc_mgr_ = std::make_unique<TestSPCManager>();

        svr_ = std::make_unique<httplib::Server>();
        RegisterQueryRoutes(*svr_, *auth_, harness_->ipool(), *embedder_,
                            *classifier_, *fusion_);
        RegisterMemoryRoutes(*svr_, *auth_, harness_->ipool(), *spc_mgr_,
                             *embedder_, *classifier_, *fusion_,
                             config_.memory);

        port_ = 19400 + (getpid() % 500);
        svr_thread_ = std::thread([this] { svr_->listen("127.0.0.1", port_); });

        httplib::Client cli("127.0.0.1", port_);
        for (int i = 0; i < 50; ++i) {
            auto res = cli.Post("/api/v1/memory/sessions");
            if (res) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    void TearDown() override {
        if (svr_) svr_->stop();
        if (svr_thread_.joinable()) svr_thread_.join();
        std::error_code ec;
        fs::remove_all(tmp_dir_, ec);
    }

    httplib::Headers AuthHeaders() {
        return {{"Authorization", "Bearer " + test_key_}};
    }

    class TestSPCManager : public SPCManager {
    public:
        TestSPCManager() : SPCManager() {}
        Status Submit(std::shared_ptr<SPCTask>) override { return Status::Ok(); }
        int CancelBySourcePath(const std::string&) override { return 0; }
        void Start() override {}
        void Stop() override {}
        size_t QueueSize() const override { return 0; }
        SPCStage GetTaskStage(int64_t) const override { return SPCStage::kQueued; }
    };

    CortrixConfig config_;
    fs::path tmp_dir_;
    std::unique_ptr<test::NsPoolHarness> harness_;
    std::unique_ptr<ApiKeyAuth> auth_;
    std::unique_ptr<OnnxEmbedder> embedder_;
    std::unique_ptr<IntentClassifier> classifier_;
    std::unique_ptr<RRFFusion> fusion_;
    std::unique_ptr<TestSPCManager> spc_mgr_;
    std::unique_ptr<httplib::Server> svr_;
    std::thread svr_thread_;
    std::string test_key_;
    int port_;
};

// SEC-MAL-001: Completely invalid JSON body
TEST_F(MalformedInputTest, InvalidJsonBody) {
    httplib::Client cli("127.0.0.1", port_);

    std::vector<std::string> invalid_bodies = {
        "not json at all",
        "{broken",
        "[1,2,",
        "",
        "null",
        "true",
        "42",
        "{\"key\": undefined}",
    };

    for (const auto& body : invalid_bodies) {
        auto res = cli.Post("/api/v1/query", AuthHeaders(),
                            body, "application/json");
        ASSERT_TRUE(res) << "No response for invalid body: " << body;
        // Should be 400 (bad request) or 422, NOT 500
        EXPECT_TRUE(res->status == 400 || res->status == 422)
            << "Status " << res->status << " for: " << body;
    }
}

// SEC-MAL-002: Oversized JSON body (1MB+ query string)
TEST_F(MalformedInputTest, OversizedJsonBody) {
    httplib::Client cli("127.0.0.1", port_);

    // 1MB query string
    std::string huge_query(1024 * 1024, 'A');
    json body;
    body["query"] = huge_query;
    body["namespace"] = "default";
    body["top_k"] = 5;

    auto res = cli.Post("/api/v1/query", AuthHeaders(),
                        body.dump(), "application/json");
    ASSERT_TRUE(res);
    // Should handle gracefully: 200 (with truncated query), 400, or 413
    EXPECT_TRUE(res->status == 200 || res->status == 400 ||
                res->status == 413 || res->status == 503)
        << "Unexpected status for oversized body: " << res->status;
}

// SEC-MAL-003: Invalid UTF-8 in query text
TEST_F(MalformedInputTest, InvalidUtf8InQuery) {
    httplib::Client cli("127.0.0.1", port_);

    // nlohmann::json rejects invalid UTF-8, so we must send raw HTTP body
    // with invalid UTF-8 bytes embedded in the JSON string.
    // This tests the server's handling of malformed UTF-8 in raw requests.
    std::string raw_body = R"({"query":"test )";
    raw_body += '\xC0';  // Invalid UTF-8 start byte
    raw_body += '\xAF';
    raw_body += R"( query","namespace":"default","top_k":5})";

    auto res = cli.Post("/api/v1/query", AuthHeaders(),
                        raw_body, "application/json");
    ASSERT_TRUE(res);
    // Server should handle gracefully: parse error (400/422) or process with
    // replacement chars. Must NOT crash (500).
    EXPECT_NE(res->status, 500)
        << "Server returned 500 for invalid UTF-8";
}

// SEC-MAL-004: Deeply nested JSON object
TEST_F(MalformedInputTest, DeeplyNestedJson) {
    httplib::Client cli("127.0.0.1", port_);

    // Build a 100-level deep nested JSON string
    std::string deep_json;
    for (int i = 0; i < 100; ++i) {
        deep_json += R"({"nested":)";
    }
    deep_json += R"({"query":"deep","namespace":"default","top_k":5})";
    for (int i = 0; i < 100; ++i) {
        deep_json += "}";
    }

    auto res = cli.Post("/api/v1/query", AuthHeaders(),
                        deep_json, "application/json");
    ASSERT_TRUE(res);
    // Should handle gracefully, not crash or hang
    EXPECT_NE(res->status, 500)
        << "Server returned 500 for deeply nested JSON";
}

// SEC-MAL-005: Null bytes in string fields
TEST_F(MalformedInputTest, NullBytesInStringFields) {
    httplib::Client cli("127.0.0.1", port_);

    // JSON with embedded null bytes in query
    std::string query_with_null = std::string("test\0injection", 14);
    json body;
    body["query"] = query_with_null;
    body["namespace"] = "default";
    body["top_k"] = 5;

    auto res = cli.Post("/api/v1/query", AuthHeaders(),
                        body.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_NE(res->status, 500)
        << "Server returned 500 for null byte in query";
}

// SEC-MAL-006: Integer overflow in top_k/limit params
TEST_F(MalformedInputTest, IntegerOverflowInParams) {
    httplib::Client cli("127.0.0.1", port_);

    std::vector<json> overflow_bodies = {
        // Extremely large top_k
        {{"query", "test"}, {"namespace", "default"}, {"top_k", 2147483647}},
        // Negative top_k
        {{"query", "test"}, {"namespace", "default"}, {"top_k", -1}},
        // Zero top_k
        {{"query", "test"}, {"namespace", "default"}, {"top_k", 0}},
        // Float where int expected
        {{"query", "test"}, {"namespace", "default"}, {"top_k", 3.14}},
    };

    for (const auto& body : overflow_bodies) {
        auto res = cli.Post("/api/v1/query", AuthHeaders(),
                            body.dump(), "application/json");
        ASSERT_TRUE(res) << "No response for: " << body.dump();
        // Should handle gracefully: 200 (clamped), 400 (rejected), or 503
        EXPECT_NE(res->status, 500)
            << "Status 500 for: " << body.dump();
    }
}

// SEC-MAL-007: Empty POST body on endpoints that expect JSON
TEST_F(MalformedInputTest, EmptyPostBody) {
    httplib::Client cli("127.0.0.1", port_);

    std::vector<std::string> endpoints = {
        "/api/v1/query",
        "/api/v1/memory/sessions",
        "/api/v1/memory/search",
        "/api/v1/memory/inject",
    };

    for (const auto& ep : endpoints) {
        auto res = cli.Post(ep.c_str(), AuthHeaders(),
                            "", "application/json");
        ASSERT_TRUE(res) << "No response for empty body to " << ep;
        // Should be 400/422, not 500
        EXPECT_NE(res->status, 500)
            << "Server returned 500 for empty body to " << ep;
    }
}

// SEC-MAL-008: Wrong content type
TEST_F(MalformedInputTest, WrongContentType) {
    httplib::Client cli("127.0.0.1", port_);

    json body;
    body["query"] = "test";
    body["namespace"] = "default";
    body["top_k"] = 5;

    // Send JSON but with wrong content type
    auto res = cli.Post("/api/v1/query", AuthHeaders(),
                        body.dump(), "text/plain");
    ASSERT_TRUE(res);
    // Server may accept or reject, but should not crash
    EXPECT_NE(res->status, 500)
        << "Server returned 500 for wrong content type";
}

}  // namespace
}  // namespace cortrix
