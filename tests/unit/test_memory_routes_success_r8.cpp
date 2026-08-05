// R8 — memory_routes.cpp EXTRACT success-body branch coverage (the 96-missed
// pool the disabled-guard tests can't reach).
//
// The existing test_memory_routes.cpp + test_memory_routes_r7b.cpp register the
// routes with MemoryServices{} defaulted (extraction == nullptr), so every
// extract handler short-circuits at the `!extraction || !enabled()` guard (503).
// To reach the SUCCESS bodies (parse → ExtractOne → 200 with extracted_memories)
// the service must be enabled. Key seam (R7 stub brief): MemoryExtractionService
// is a concrete class (can't subclass), but enabled() == (llm_ != nullptr) and
// ILlmClient is a PURE interface — so a tiny StubLlmClient whose Chat() returns a
// fixed memory extraction JSON array makes enabled()=true and drives the real
// ExtractOne end to end against a fresh in-memory namespace.
//
// Harness mirrors test_memory_routes.cpp (real httplib server + NsPoolHarness +
// ApiKeyAuth + Client), self-contained here (no shared fixture header), per the
// test_gc_manager_extra duplication precedent.
//
// NOTE: integration-style (server+client). syntax-checked here; endpoint status
// codes are build-verified by the lead's ctest run.
#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include "httplib.h"
#include <nlohmann/json.hpp>

#include "cortrix/auth/api_key_auth.h"
#include "cortrix/config/config.h"
#include "cortrix/llm/i_llm_client.h"
#include "cortrix/memory/memory_config.h"
#include "cortrix/memory/memory_extraction_service.h"
#include "cortrix/memory/memory_extractor.h"
#include "cortrix/memory/memory_queue.h"
#include "cortrix/memory/memory_routes.h"
#include "cortrix/query/intent_classifier.h"
#include "cortrix/query/rrf_fusion.h"
#include "cortrix/spc/onnx_embedder.h"

#include "mock_spc_manager.h"
#include "ns_pool_test_helper.h"

namespace cortrix {
namespace {

using json = nlohmann::json;

// Stub LLM: Chat() returns a fixed memory extraction JSON array (the schema
// MemoryExtractor::ParseExtractionJson expects — an array of objects each with a
// string "content" + optional numeric "confidence"). status=ok so the extractor
// proceeds to parse + persist. Prompt is ignored (deterministic output).
class StubLlmClient : public llm::ILlmClient {
public:
    llm::ChatCompletionResponse Chat(const std::string& /*prompt*/,
                                     const llm::LlmCallConfig& /*config*/) override {
        llm::ChatCompletionResponse r;
        r.status = Status::Ok();
        // One extracted fact — kept minimal so the contradiction step finds no
        // candidates in the fresh NS store (no second judge round-trip needed).
        r.content = R"([{"content":"user prefers dark mode","memory_type":"preference","confidence":0.9}])";
        r.model = "stub-llm";
        r.finish_reason = "stop";
        r.prompt_tokens = 10;
        r.completion_tokens = 8;
        return r;
    }
};

class MemoryRoutesSuccessTest : public ::testing::Test {
protected:
    void SetUp() override {
        tmp_dir_ = "/tmp/cortrix_test_memroutes_succ_" + std::to_string(getpid());
        system(("mkdir -p " + tmp_dir_).c_str());

        config_.auth.enabled = true;
        config_.ns.data_dir = tmp_dir_;

        const std::string test_key = "test-api-key-12345";
        ApiKeyConfig key_config;
        key_config.key_hash = ApiKeyAuth::HashKey(test_key);
        key_config.tenant_id = "test-tenant";
        key_config.permissions = 7;  // read + write + admin
        config_.auth.api_keys.push_back(key_config);
        auth_.LoadKeys(config_.auth.api_keys);

        harness_ = std::make_unique<cortrix::test::NsPoolHarness>(tmp_dir_ + "/pool");
        ASSERT_TRUE(harness_->Admit("default").ok());

        port_ = 19280 + (getpid() % 1000);

        embedder_ = std::make_unique<OnnxEmbedder>("", 128);
        embedder_->Init();
        LlmConfig llm_cfg;
        classifier_ = std::make_unique<IntentClassifier>(llm_cfg);
        fusion_ = std::make_unique<RRFFusion>();

        // The enabled memory extraction service: a non-null StubLlmClient → enabled()==true.
        extraction_ = std::make_unique<memory::MemoryExtractionService>(
            harness_->ipool(), std::make_shared<StubLlmClient>(), *embedder_,
            /*op_logger=*/nullptr, memory::MemoryExtractorConfig{},
            memory::MemoryQueue::Config{});

        svr_ = std::make_unique<httplib::Server>();
        MemoryServices services;
        services.extraction = extraction_.get();  // non-null → extract routes enabled
        RegisterMemoryRoutes(*svr_, auth_, harness_->ipool(), mock_spc_,
                             *embedder_, *classifier_, *fusion_, config_.memory,
                             services);

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
        extraction_.reset();
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
    std::unique_ptr<memory::MemoryExtractionService> extraction_;
    std::unique_ptr<httplib::Server> svr_;
    std::thread svr_thread_;
    int port_ = 0;
};

// POST /memory/extract with content → enabled service → parse → ExtractOne →
// 200 with extracted_memories (the success body, L165-211 of the handler).
TEST_F(MemoryRoutesSuccessTest, ExtractSingleReturnsExtractedMemories) {
    httplib::Client cli("127.0.0.1", port_);
    json body{{"ns", "default"},
              {"session_id", "sess-1"},
              {"content", "user: I like dark mode\nassistant: noted"}};
    auto res = cli.Post("/api/v1/memory/extract", AuthHeaders(), body.dump(),
                        "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200) << "body: " << res->body;
    auto j = json::parse(res->body, nullptr, /*allow_exceptions=*/false);
    ASSERT_FALSE(j.is_discarded());
    EXPECT_TRUE(j.contains("extracted_memories"));
    EXPECT_TRUE(j["extracted_memories"].is_array());
}

// POST /memory/extract using the query_text/response_text pair (the `else` branch
// of the content-vs-pair build at L182-184) instead of an explicit content field.
TEST_F(MemoryRoutesSuccessTest, ExtractFromQueryResponsePair) {
    httplib::Client cli("127.0.0.1", port_);
    json body{{"ns", "default"},
              {"session_id", "sess-2"},
              {"query_text", "what mode do I prefer"},
              {"response_text", "you prefer dark mode"}};
    auto res = cli.Post("/api/v1/memory/extract", AuthHeaders(), body.dump(),
                        "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200) << "body: " << res->body;
}

// POST /memory/extract missing ns → 400 (the ns-required arm, BEFORE ExtractOne,
// reached only because the service is enabled — under a disabled service the 503
// guard would mask it).
TEST_F(MemoryRoutesSuccessTest, ExtractMissingNsReturns400) {
    httplib::Client cli("127.0.0.1", port_);
    json body{{"content", "no ns here"}};
    auto res = cli.Post("/api/v1/memory/extract", AuthHeaders(), body.dump(),
                        "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400) << "body: " << res->body;
}

// POST /memory/extract with malformed JSON → 400 (the parse-catch arm, reached
// only under an enabled service; under disabled the 503 guard runs first).
TEST_F(MemoryRoutesSuccessTest, ExtractInvalidJsonReturns400) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Post("/api/v1/memory/extract", AuthHeaders(),
                        "{not valid json", "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400) << "body: " << res->body;
}

// POST /memory/extract/batch → PartialSuccessById 200 with per-item results (the
// batch success body + the interactions[] loop, L230-254).
TEST_F(MemoryRoutesSuccessTest, ExtractBatchReturnsPerItemResults) {
    httplib::Client cli("127.0.0.1", port_);
    json body{
        {"ns", "default"},
        {"interactions", json::array({
            json{{"session_id", "b1"}, {"content", "user: fact one\nassistant: ok"}},
            json{{"session_id", "b2"}, {"query_text", "q"}, {"response_text", "r"}},
        })}};
    auto res = cli.Post("/api/v1/memory/extract/batch", AuthHeaders(), body.dump(),
                        "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200) << "body: " << res->body;
    auto j = json::parse(res->body, nullptr, false);
    ASSERT_FALSE(j.is_discarded());
    EXPECT_TRUE(j.contains("results"));
    EXPECT_TRUE(j.contains("succeeded"));
    EXPECT_TRUE(j.contains("failed"));
}

// POST /memory/extract/batch missing interactions[] → 400 (the validation arm).
TEST_F(MemoryRoutesSuccessTest, ExtractBatchMissingInteractionsReturns400) {
    httplib::Client cli("127.0.0.1", port_);
    json body{{"ns", "default"}};  // no interactions[]
    auto res = cli.Post("/api/v1/memory/extract/batch", AuthHeaders(), body.dump(),
                        "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400) << "body: " << res->body;
}

// POST /memory/extract/run_for_ns → 202 scheduled (the backfill success body,
// reached under an enabled service; L273-277).
TEST_F(MemoryRoutesSuccessTest, ExtractRunForNsReturnsScheduled) {
    httplib::Client cli("127.0.0.1", port_);
    json body{{"ns", "default"}};
    auto res = cli.Post("/api/v1/memory/extract/run_for_ns", AuthHeaders(), body.dump(),
                        "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 202) << "body: " << res->body;
    auto j = json::parse(res->body, nullptr, false);
    ASSERT_FALSE(j.is_discarded());
    EXPECT_EQ(j.value("status", ""), "scheduled");
}

// POST /memory/extract/run_for_ns missing ns → 400 (the ns-required arm under
// the enabled service).
TEST_F(MemoryRoutesSuccessTest, ExtractRunForNsMissingNsReturns400) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Post("/api/v1/memory/extract/run_for_ns", AuthHeaders(),
                        "{}", "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400) << "body: " << res->body;
}

}  // namespace
}  // namespace cortrix
