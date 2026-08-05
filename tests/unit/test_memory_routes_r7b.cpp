// R7-1d — branch-coverage supplements for memory_routes.cpp (63.87% branch).
//
// The existing test_memory_routes.cpp (2219 lines) exhaustively covers the
// sessions / search / inject / auth surfaces, but never POSTs to the memory extraction
// extract routes or the invalidation-audit routes. Those are registered by
// RegisterMemoryRoutes with MemoryServices{} defaulted (extraction == nullptr),
// so the extract handlers' "service disabled" guard arms + the invalidations
// list body sit uncovered. These tests hit exactly those reachable arms with
// the SAME harness shape (no new service stub needed — null extraction is the
// disabled path under test).
//
// (The extract/transparency success BODIES need a live MemoryExtractionService
// stub + seeded façade state and are deferred — see the note reported to lead.)
//
// Strategy mirrors test_memory_routes.cpp: a real httplib::Server with the routes
// registered over a real namespace pool (NsPoolHarness) + a MockSPCManager, driven via
// httplib::Client. Harness setup is duplicated here (no shared fixture header),
// following the test_gc_manager_extra.cpp precedent.
#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include "httplib.h"
#include <nlohmann/json.hpp>

#include "cortrix/auth/api_key_auth.h"
#include "cortrix/memory/memory_config.h"
#include "cortrix/memory/memory_routes.h"
#include "cortrix/query/intent_classifier.h"
#include "cortrix/query/rrf_fusion.h"
#include "cortrix/spc/onnx_embedder.h"

#include "mock_spc_manager.h"
#include "ns_pool_test_helper.h"

namespace cortrix {
namespace {

using json = nlohmann::json;

// Lean fixture: register the memory routes with the DEFAULT MemoryServices{}
// (extraction == nullptr), which is exactly the "extraction disabled" config the
// uncovered guard arms gate on. A real namespace pool admits the "default" namespace so
// the per-request façade Acquire() succeeds for routes that reach it.
class MemoryRoutesR7bTest : public ::testing::Test {
protected:
    void SetUp() override {
        tmp_dir_ = "/tmp/cortrix_test_memroutes_r7b_" + std::to_string(getpid());
        system(("mkdir -p " + tmp_dir_).c_str());

        config_.auth.enabled = true;
        config_.ns.data_dir = tmp_dir_;

        const std::string test_key = "test-api-key-12345";
        ApiKeyConfig key_config;
        key_config.key_hash = ApiKeyAuth::HashKey(test_key);
        key_config.tenant_id = "test-tenant";
        key_config.permissions = 7;  // read + write + admin (revoke needs admin)
        config_.auth.api_keys.push_back(key_config);
        auth_.LoadKeys(config_.auth.api_keys);

        harness_ = std::make_unique<cortrix::test::NsPoolHarness>(tmp_dir_ + "/pool");
        ASSERT_TRUE(harness_->Admit("default").ok());

        port_ = 19180 + (getpid() % 1000);

        embedder_ = std::make_unique<OnnxEmbedder>("", 128);
        embedder_->Init();
        LlmConfig llm_cfg;
        classifier_ = std::make_unique<IntentClassifier>(llm_cfg);
        fusion_ = std::make_unique<RRFFusion>();

        svr_ = std::make_unique<httplib::Server>();
        // services defaulted ({}) → extraction == nullptr (the disabled path).
        RegisterMemoryRoutes(*svr_, auth_, harness_->ipool(), mock_spc_,
                             *embedder_, *classifier_, *fusion_, config_.memory);

        svr_thread_ = std::thread([this] { svr_->listen("127.0.0.1", port_); });

        httplib::Client cli("127.0.0.1", port_);
        for (int i = 0; i < 50; ++i) {
            auto res = cli.Post("/api/v1/memory/sessions", "{}", "application/json");
            if (res) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    void TearDown() override {
        svr_->stop();
        if (svr_thread_.joinable()) svr_thread_.join();
        system(("rm -rf " + tmp_dir_).c_str());
    }

    httplib::Headers AuthHeaders() {
        return {{"Authorization", "Bearer test-api-key-12345"}};
    }

    std::string tmp_dir_;
    CortrixConfig config_;
    ApiKeyAuth auth_;
    std::unique_ptr<cortrix::test::NsPoolHarness> harness_;
    cortrix::testing::MockSPCManager mock_spc_;
    std::unique_ptr<OnnxEmbedder> embedder_;
    std::unique_ptr<IntentClassifier> classifier_;
    std::unique_ptr<RRFFusion> fusion_;
    std::unique_ptr<httplib::Server> svr_;
    std::thread svr_thread_;
    int port_ = 0;
};

// POST /memory/extract with extraction disabled (null service) → 503 + the
// CX_ERR_MEMEXTRACT_LLM_DISABLED agent error (the `!extraction || !enabled()` guard).
TEST_F(MemoryRoutesR7bTest, ExtractDisabledReturns503) {
    httplib::Client cli("127.0.0.1", port_);
    json body{{"ns", "default"}, {"content", "user: hi\nassistant: hello"}};
    auto res = cli.Post("/api/v1/memory/extract", AuthHeaders(), body.dump(),
                        "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 503);
    EXPECT_NE(res->body.find("CX_ERR_MEMEXTRACT_LLM_DISABLED"), std::string::npos);
}

// POST /memory/extract/batch disabled → 503 (the batch route's guard arm).
TEST_F(MemoryRoutesR7bTest, ExtractBatchDisabledReturns503) {
    httplib::Client cli("127.0.0.1", port_);
    json body{{"ns", "default"}, {"interactions", json::array()}};
    auto res = cli.Post("/api/v1/memory/extract/batch", AuthHeaders(), body.dump(),
                        "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 503);
    EXPECT_NE(res->body.find("CX_ERR_MEMEXTRACT_LLM_DISABLED"), std::string::npos);
}

// POST /memory/extract/run_for_ns disabled → 503 (the backfill route's guard arm).
TEST_F(MemoryRoutesR7bTest, ExtractRunForNsDisabledReturns503) {
    httplib::Client cli("127.0.0.1", port_);
    json body{{"ns", "default"}};
    auto res = cli.Post("/api/v1/memory/extract/run_for_ns", AuthHeaders(), body.dump(),
                        "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 503);
    EXPECT_NE(res->body.find("CX_ERR_MEMEXTRACT_LLM_DISABLED"), std::string::npos);
}

// POST /memory/invalidations/{id}/revoke with null extraction → 503 (the
// `!extraction` arm; distinct from the 501-not-implemented arm that runs when
// extraction is present).
TEST_F(MemoryRoutesR7bTest, RevokeWithoutExtractionReturns503) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Post("/api/v1/memory/invalidations/blk_123/revoke", AuthHeaders(),
                        "{}", "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 503);
    EXPECT_NE(res->body.find("CX_ERR_MEMEXTRACT_LLM_DISABLED"), std::string::npos);
}

// GET /memory/invalidations → 200 with the canonical operation_log pointer (the
// audit-list body, which has no extraction dependency and was never hit).
TEST_F(MemoryRoutesR7bTest, InvalidationsListReturnsSourcePointer) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Get("/api/v1/memory/invalidations", AuthHeaders());
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);
    auto j = json::parse(res->body, nullptr, /*allow_exceptions=*/false);
    ASSERT_FALSE(j.is_discarded());
    EXPECT_TRUE(j.contains("invalidations"));
    EXPECT_TRUE(j["invalidations"].is_array());
    EXPECT_NE(j.value("source", "").find("memory_invalidate"), std::string::npos);
}

// POST /memory/extract with a malformed JSON body — but since extraction is
// disabled, the guard fires BEFORE the parse, so this still 503s (confirms the
// guard precedence; the invalid-JSON 400 arm needs a live extraction service and
// is deferred).
TEST_F(MemoryRoutesR7bTest, ExtractDisabledTakesPrecedenceOverBadJson) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Post("/api/v1/memory/extract", AuthHeaders(),
                        "{not valid json", "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 503) << "disabled guard runs before JSON parse";
}

}  // namespace
}  // namespace cortrix
