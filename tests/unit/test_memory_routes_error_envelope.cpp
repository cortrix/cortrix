// Error envelope of the memory routes: every error response carries the same
// request-scoped fields as the shared error writer (X-Request-Id header,
// error.request_id equal to it, error.timestamp, the error.retry_after_ms key), and
// an extraction failure is sent with the MEM02 registry row of its code (HTTP
// status, category, retryable, retry_after_ms) rather than a fixed
// 500/transient/not-retryable.
//
// Harness mirrors test_memory_routes_success_r8.cpp: real routes over an
// NsPoolHarness pool, driven via httplib::Client. The LLM is a stub whose
// response is chosen per test; a null LLM leaves the extraction service disabled.

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

// Stub LLM returning a fixed status and content.
class FixedLlmClient : public llm::ILlmClient {
public:
    FixedLlmClient(Status status, std::string content)
        : status_(std::move(status)), content_(std::move(content)) {}
    llm::ChatCompletionResponse Chat(const std::string& /*prompt*/,
                                     const llm::LlmCallConfig& /*config*/) override {
        llm::ChatCompletionResponse r;
        r.status = status_;
        r.content = content_;
        r.model = "stub-llm";
        r.finish_reason = "stop";
        return r;
    }

private:
    Status status_;
    std::string content_;
};

class MemoryRoutesErrorEnvelopeTest : public ::testing::Test {
protected:
    void TearDown() override {
        if (svr_) svr_->stop();
        if (svr_thread_.joinable()) svr_thread_.join();
        extraction_.reset();
        if (!tmp_dir_.empty()) system(("rm -rf " + tmp_dir_).c_str());
    }

    // Start the routes. `llm` null → extraction service absent (disabled).
    void Start(std::shared_ptr<llm::ILlmClient> llm) {
        tmp_dir_ = "/tmp/cortrix_test_memroutes_env_" + std::to_string(getpid());
        system(("mkdir -p " + tmp_dir_).c_str());

        config_.auth.enabled = true;
        config_.ns.data_dir = tmp_dir_;
        ApiKeyConfig key_config;
        key_config.key_hash = ApiKeyAuth::HashKey("test-api-key-12345");
        key_config.tenant_id = "test-tenant";
        key_config.permissions = 7;
        config_.auth.api_keys.push_back(key_config);
        auth_.LoadKeys(config_.auth.api_keys);

        harness_ = std::make_unique<cortrix::test::NsPoolHarness>(tmp_dir_ + "/pool");
        ASSERT_TRUE(harness_->Admit("default").ok());

        embedder_ = std::make_unique<OnnxEmbedder>("", 128);
        embedder_->Init();
        LlmConfig llm_cfg;
        classifier_ = std::make_unique<IntentClassifier>(llm_cfg);
        fusion_ = std::make_unique<RRFFusion>();

        MemoryServices services;
        if (llm) {
            extraction_ = std::make_unique<memory::MemoryExtractionService>(
                harness_->ipool(), std::move(llm), *embedder_,
                /*op_logger=*/nullptr, memory::MemoryExtractorConfig{},
                memory::MemoryQueue::Config{});
            services.extraction = extraction_.get();
        }

        svr_ = std::make_unique<httplib::Server>();
        RegisterMemoryRoutes(*svr_, auth_, harness_->ipool(), mock_spc_, *embedder_,
                             *classifier_, *fusion_, config_.memory, services);
        port_ = svr_->bind_to_any_port("127.0.0.1");
        ASSERT_GT(port_, 0);
        svr_thread_ = std::thread([this] { svr_->listen_after_bind(); });
        httplib::Client cli("127.0.0.1", port_);
        for (int i = 0; i < 50; ++i) {
            if (cli.Get("/__memory_env_ready_probe")) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

    httplib::Result PostExtract(const std::string& body) {
        httplib::Client cli("127.0.0.1", port_);
        return cli.Post("/api/v1/memory/extract",
                        httplib::Headers{{"Authorization", "Bearer test-api-key-12345"}},
                        body, "application/json");
    }

    static std::string ExtractBody() {
        return json{{"ns", "default"}, {"content", "user: hi\nassistant: hello"}}.dump();
    }

    // The request-scoped fields every error response carries.
    static void ExpectRequestFields(const httplib::Result& res) {
        ASSERT_TRUE(res);
        const std::string header = res->get_header_value("X-Request-Id");
        EXPECT_EQ(header.size(), 32u) << "X-Request-Id header missing";
        const json j = json::parse(res->body);
        ASSERT_TRUE(j.contains("error"));
        EXPECT_EQ(j["error"].value("request_id", ""), header);
        EXPECT_TRUE(j["error"].contains("timestamp"));
        EXPECT_TRUE(j["error"].contains("retry_after_ms"));
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

TEST_F(MemoryRoutesErrorEnvelopeTest, LlmDisabledCarriesRequestFields) {
    Start(nullptr);
    auto res = PostExtract(ExtractBody());
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 503);
    ExpectRequestFields(res);
    const json e = json::parse(res->body)["error"];
    EXPECT_EQ(e["code"], "CX_ERR_MEM02_LLM_DISABLED");
    EXPECT_EQ(e["category"], "permanent");
    EXPECT_FALSE(e["retryable"].get<bool>());
    EXPECT_TRUE(e["retry_after_ms"].is_null());
}

TEST_F(MemoryRoutesErrorEnvelopeTest, BatchLlmDisabledCarriesRequestFields) {
    Start(nullptr);
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Post("/api/v1/memory/extract/batch",
                        httplib::Headers{{"Authorization", "Bearer test-api-key-12345"}},
                        R"({"interactions":[]})", "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 503);
    ExpectRequestFields(res);
}

TEST_F(MemoryRoutesErrorEnvelopeTest, StatusErrorCarriesRequestFields) {
    Start(std::make_shared<FixedLlmClient>(Status::Ok(), "[]"));
    auto res = PostExtract(R"({"content":"no namespace"})");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
    ExpectRequestFields(res);
}

TEST_F(MemoryRoutesErrorEnvelopeTest, LlmTimeoutIs504TimeoutRetryable) {
    Start(std::make_shared<FixedLlmClient>(Status::Unavailable("deadline exceeded"), ""));
    auto res = PostExtract(ExtractBody());
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 504) << res->body;
    ExpectRequestFields(res);
    const json e = json::parse(res->body)["error"];
    EXPECT_EQ(e["code"], "CX_ERR_MEM02_EXTRACT_LLM_TIMEOUT");
    EXPECT_EQ(e["category"], "timeout");
    EXPECT_TRUE(e["retryable"].get<bool>());
    EXPECT_EQ(e["retry_after_ms"], 5000);
    EXPECT_TRUE(e["structured_data"].contains("interaction_id"));
}

TEST_F(MemoryRoutesErrorEnvelopeTest, InvalidLlmOutputIs500TransientRetryable) {
    Start(std::make_shared<FixedLlmClient>(Status::Ok(), "this is not json"));
    auto res = PostExtract(ExtractBody());
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 500) << res->body;
    ExpectRequestFields(res);
    const json e = json::parse(res->body)["error"];
    EXPECT_EQ(e["code"], "CX_ERR_MEM02_EXTRACT_INVALID_OUTPUT");
    EXPECT_EQ(e["category"], "transient");
    EXPECT_TRUE(e["retryable"].get<bool>());
    EXPECT_EQ(e["retry_after_ms"], 5000);
}

TEST_F(MemoryRoutesErrorEnvelopeTest, LlmBudgetIs429QuotaNotRetryable) {
    Start(std::make_shared<FixedLlmClient>(Status::PermissionDenied("budget"), ""));
    auto res = PostExtract(ExtractBody());
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 429) << res->body;
    ExpectRequestFields(res);
    const json e = json::parse(res->body)["error"];
    EXPECT_EQ(e["code"], "CX_ERR_MEM02_EXTRACT_BUDGET_EXCEEDED");
    EXPECT_EQ(e["category"], "quota");
    EXPECT_FALSE(e["retryable"].get<bool>());
}

}  // namespace
}  // namespace cortrix
