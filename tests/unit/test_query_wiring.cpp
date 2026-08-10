// query_wiring.cpp — behavioral coverage of the F39/F36 wiring contracts.
//
// Scope note: DecisionOf / ReadRouteOverride / ResolveRagFusionConfig /
// RecordQueryTrace live in an anonymous namespace inside query_wiring.cpp. The
// focused tests below pin the contracts at the real components the closure delegates
// to. The final fixture also assembles CrossNsQueryWiring itself behind an ephemeral
// loopback HTTP server, using a temporary store, stub embedder, deterministic index,
// and scripted LLM double; it requires no external service, model, or network access.
//
//   * ReadRouteOverride -> CX_ERR_F39_FORCE_ROUTE_INVALID : the override flows into
//     QueryComplexityClassifier::RouteAndUpdateContext(ctx, route); a bad token
//     returns the F39 permanent error and leaves ctx untouched (exactly what the
//     closure surfaces as a 400).
//   * DecisionOf(routing_path, source) : RouteAndUpdateContext writes the
//     routing_path + routing_decision_source the closure maps to a
//     QueryRouterMetrics::Decision, and itself records that decision. We assert the
//     written fields + the metric the closure reads from them.
//   * ResolveRagFusionConfig : the resolved RagFusionConfig defaults to
//     enabled=false (topic 3) and only the per-request opt-in flips it on — the
//     exact gate the closure's use_rag_fusion computes.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "httplib.h"

#include "cortrix/auth/api_key_auth.h"
#include "cortrix/llm/i_llm_client.h"
#include "cortrix/memory/memory_block_adapter.h"
#include "cortrix/query/complexity_config.h"
#include "cortrix/query/heuristic_complexity_backend.h"
#include "cortrix/query/query_complexity_classifier.h"
#include "cortrix/query/query_context.h"
#include "cortrix/query/query_router_metrics.h"
#include "cortrix/query/query_wiring.h"
#include "cortrix/query/rag_fusion_types.h"
#include "cortrix/query/rrf_fusion.h"
#include "cortrix/query/router_error.h"
#include "cortrix/resource/namespace_facade.h"
#include "cortrix/spc/onnx_embedder.h"
#include "cortrix/tenant/permission_service.h"

#include "fake_doc_discovery_deps.h"
#include "mock_llm_client.h"

namespace cortrix::query {
namespace {

using json = nlohmann::json;
using ::testing::_;
using ::testing::Invoke;
using ::testing::NiceMock;

QueryComplexityClassifier MakeClassifier(const ComplexityConfig& cfg = {}) {
    return QueryComplexityClassifier(
        std::make_shared<HeuristicComplexityBackend>(/*reported_confidence=*/0.9f),
        cfg);
}

class QueryWiringTest : public ::testing::Test {
protected:
    void SetUp() override { QueryRouterMetrics::Instance().ResetForTest(); }
};

// ===========================================================================
// ReadRouteOverride -> CX_ERR_F39_FORCE_ROUTE_INVALID
// ===========================================================================

// A bad ?route= override is the F39 permanent error; ctx is left untouched so the
// closure can surface the Agent-friendly 400 without a half-written routing_path.
TEST_F(QueryWiringTest, InvalidRouteOverrideYieldsF39Error) {
    auto classifier = MakeClassifier();
    QueryContext ctx;
    ctx.query = "anything";
    Status s = classifier.RouteAndUpdateContext(ctx, std::optional<std::string>("banana"));
    ASSERT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kInvalidArgument);
    EXPECT_NE(s.message().find("CX_ERR_F39_FORCE_ROUTE_INVALID"), std::string::npos);
    // ctx untouched on the invalid-override path.
    EXPECT_TRUE(ctx.routing_path.empty());
    EXPECT_TRUE(ctx.routing_decision_source.empty());
}

// Each valid override token is honored verbatim with source = "force_route"
// (the §6.1 step 1 path the closure relies on).
TEST_F(QueryWiringTest, ValidRouteOverridesHonored) {
    for (const char* route : {"simple", "complex", "chat"}) {
        auto classifier = MakeClassifier();
        QueryContext ctx;
        ctx.query = "what is the revenue";
        Status s = classifier.RouteAndUpdateContext(
            ctx, std::optional<std::string>(route));
        ASSERT_TRUE(s.ok()) << route;
        EXPECT_EQ(ctx.routing_path, route);
        EXPECT_EQ(ctx.routing_decision_source, "force_route");
    }
}

// route="auto" is NOT an override — it falls through to the classifier, so the
// resolved path comes from the heuristic, not "force_route".
TEST_F(QueryWiringTest, RouteAutoFallsThroughToClassifier) {
    auto classifier = MakeClassifier();
    QueryContext ctx;
    ctx.query = "short query";  // < complex threshold, not a greeting -> simple
    Status s = classifier.RouteAndUpdateContext(ctx, std::optional<std::string>("auto"));
    ASSERT_TRUE(s.ok());
    EXPECT_NE(ctx.routing_decision_source, "force_route");
    EXPECT_EQ(ctx.routing_path, "simple");
}

// No override supplied (nullopt) also falls through to the classifier.
TEST_F(QueryWiringTest, NoOverrideUsesClassifier) {
    auto classifier = MakeClassifier();
    QueryContext ctx;
    ctx.query = "hi";  // greeting -> chat via rule guard
    Status s = classifier.RouteAndUpdateContext(ctx, std::nullopt);
    ASSERT_TRUE(s.ok());
    EXPECT_EQ(ctx.routing_path, "chat");
    EXPECT_EQ(ctx.routing_decision_source, "rule");
}

// The F39 error carries the structured-data key the closure's 400 body needs.
TEST_F(QueryWiringTest, ForceRouteInvalidErrorHasStructuredKey) {
    const auto& keys = RequiredStructuredDataKeys(RouterErrorCode::kForceRouteInvalid);
    ASSERT_EQ(keys.size(), 1u);
    EXPECT_EQ(keys[0], "invalid_route_value");
    EXPECT_STREQ(RouterErrorCodeString(RouterErrorCode::kForceRouteInvalid),
                 "CX_ERR_F39_FORCE_ROUTE_INVALID");
}

// ===========================================================================
// DecisionOf : routing_path/source -> QueryRouterMetrics::Decision
// ===========================================================================
//
// These reproduce the DecisionOf truth table at the source of truth: the values
// RouteAndUpdateContext writes, plus the metric it records. The closure calls
// DecisionOf(qctx.routing_path, qctx.routing_decision_source) on exactly these.

// A force_route="simple" => routing_path "simple", which DecisionOf maps to kSimple
// (and RouteAndUpdateContext records it).
TEST_F(QueryWiringTest, DecisionSimpleRecorded) {
    auto classifier = MakeClassifier();
    QueryContext ctx;
    ctx.query = "q";
    ASSERT_TRUE(classifier.RouteAndUpdateContext(
                    ctx, std::optional<std::string>("simple")).ok());
    EXPECT_EQ(ctx.routing_path, "simple");
    EXPECT_EQ(QueryRouterMetrics::Instance().DecisionCount(
                  QueryRouterMetrics::Decision::kSimple), 1u);
}

// A greeting routes to chat (source "rule") => DecisionOf maps to kChat.
TEST_F(QueryWiringTest, DecisionChatRecorded) {
    auto classifier = MakeClassifier();
    QueryContext ctx;
    ctx.query = "thanks";
    ASSERT_TRUE(classifier.RouteAndUpdateContext(ctx, std::nullopt).ok());
    EXPECT_EQ(ctx.routing_path, "chat");
    EXPECT_EQ(QueryRouterMetrics::Instance().DecisionCount(
                  QueryRouterMetrics::Decision::kChat), 1u);
}

// A force_route="complex" => routing_path "complex" => kComplex.
TEST_F(QueryWiringTest, DecisionComplexRecorded) {
    auto classifier = MakeClassifier();
    QueryContext ctx;
    ctx.query = "q";
    ASSERT_TRUE(classifier.RouteAndUpdateContext(
                    ctx, std::optional<std::string>("complex")).ok());
    EXPECT_EQ(ctx.routing_path, "complex");
    EXPECT_EQ(QueryRouterMetrics::Instance().DecisionCount(
                  QueryRouterMetrics::Decision::kComplex), 1u);
}

// A fail-safe source (classifier unavailable) => DecisionOf returns kFallback. We
// force the L2 path by injecting a NULL backend (IsAvailable()==false).
TEST_F(QueryWiringTest, DecisionFallbackOnUnavailableBackend) {
    QueryComplexityClassifier classifier(/*backend=*/nullptr, ComplexityConfig{});
    QueryContext ctx;
    ctx.query = "some retrievable question text";
    ASSERT_TRUE(classifier.RouteAndUpdateContext(ctx, std::nullopt).ok());
    EXPECT_EQ(ctx.routing_path, "complex");  // fail-safe to Complex
    EXPECT_EQ(ctx.routing_decision_source, "classifier_unavailable");
    // DecisionOf maps this source -> kFallback (the §10 fallback bucket).
    EXPECT_EQ(QueryRouterMetrics::Instance().DecisionCount(
                  QueryRouterMetrics::Decision::kFallback), 1u);
}

// The Decision enum label strings the metric renderer uses (the closure's bucket
// names) are stable.
TEST_F(QueryWiringTest, DecisionLabelStringsStable) {
    EXPECT_STREQ(ToString(QueryRouterMetrics::Decision::kSimple), "simple");
    EXPECT_STREQ(ToString(QueryRouterMetrics::Decision::kComplex), "complex");
    EXPECT_STREQ(ToString(QueryRouterMetrics::Decision::kChat), "chat");
    EXPECT_STREQ(ToString(QueryRouterMetrics::Decision::kFallback), "fallback");
}

// ===========================================================================
// ResolveRagFusionConfig : default-off + per-request opt-in semantics
// ===========================================================================

// The closure's ResolveRagFusionConfig starts from a default RagFusionConfig
// (enabled=false, topic 3) and only flips enabled when the request opts in. We pin
// the default the resolver builds on.
TEST_F(QueryWiringTest, RagFusionConfigDefaultsDisabled) {
    RagFusionConfig cfg;  // exactly what ResolveRagFusionConfig starts with
    EXPECT_FALSE(cfg.enabled);
    EXPECT_EQ(cfg.variant_count, 3);  // topic 1 N=3 default kept
    EXPECT_EQ(cfg.rrf_k, 60);
    EXPECT_EQ(cfg.candidate_multiplier, 1);
    EXPECT_EQ(cfg.max_candidates, 50);
    EXPECT_FALSE(cfg.final_rerank);
    EXPECT_EQ(cfg.activation_policy, "always");
    EXPECT_NEAR(cfg.activation_score_margin, 0.0f, 1e-6f);
    EXPECT_EQ(cfg.activation_min_results, 2);
}

// (Removed: RagFusionGateOffWhenConfigDisabled + RagFusionGateOnWhenEnabledComplexAndLlm
// re-implemented the closure's gate expression on string/bool literals inside
// the test — `std::string("complex") == "complex"` cannot fail and the real
// closure never ran. The gate is now pinned on the PRODUCTION closure by the
// QueryWiringHttpCoverage live tests below: gate-on inside
// RequestMatrixExercisesProductionClosure (rag_fusion.active=true), gate-off
// per leg in RagFusionGateStaysOffOnSimpleRouteAndWithoutOptIn, and the [R7]
// null-LLM leg in RagFusionGateOffWithoutLlm.)

// ValidateRagFusionConfig accepts the default and rejects an out-of-range count
// (the config the resolver hands downstream must be valid).
TEST_F(QueryWiringTest, RagFusionConfigValidation) {
    RagFusionConfig ok;
    EXPECT_TRUE(ValidateRagFusionConfig(ok));
    RagFusionConfig bad;
    bad.variant_count = 99;  // > [1,10]
    std::string field, range;
    EXPECT_FALSE(ValidateRagFusionConfig(bad, &field, &range));
    EXPECT_EQ(field, "variant_count");
}

TEST_F(QueryWiringTest, RagFusionConfigValidationRejectsExpandedCandidateBounds) {
    RagFusionConfig bad_multiplier;
    bad_multiplier.candidate_multiplier = kRagFusionCandidateMultiplierMax + 1;
    std::string field, range;
    EXPECT_FALSE(ValidateRagFusionConfig(bad_multiplier, &field, &range));
    EXPECT_EQ(field, "candidate_multiplier");

    RagFusionConfig bad_candidates;
    bad_candidates.max_candidates = kRagFusionMaxCandidatesMax + 1;
    EXPECT_FALSE(ValidateRagFusionConfig(bad_candidates, &field, &range));
    EXPECT_EQ(field, "max_candidates");

    RagFusionConfig bad_policy;
    bad_policy.activation_policy = "sometimes";
    EXPECT_FALSE(ValidateRagFusionConfig(bad_policy, &field, &range));
    EXPECT_EQ(field, "activation_policy");
}

// v1.0.13 fusion-policy knobs: bounds enforced, edge values accepted.
TEST_F(QueryWiringTest, RagFusionConfigValidationFusionPolicyBounds) {
    std::string field, range;

    RagFusionConfig bad_original;
    bad_original.fusion_original_weight = 0.0f;  // < 0.1 floor
    EXPECT_FALSE(ValidateRagFusionConfig(bad_original, &field, &range));
    EXPECT_EQ(field, "fusion_original_weight");

    RagFusionConfig bad_variant;
    bad_variant.fusion_variant_weight = kRagFusionFusionWeightMax + 1.0f;
    EXPECT_FALSE(ValidateRagFusionConfig(bad_variant, &field, &range));
    EXPECT_EQ(field, "fusion_variant_weight");

    RagFusionConfig bad_anchor;
    bad_anchor.fusion_anchor_max = kRagFusionAnchorMaxMax + 1;
    EXPECT_FALSE(ValidateRagFusionConfig(bad_anchor, &field, &range));
    EXPECT_EQ(field, "fusion_anchor_max");

    RagFusionConfig edges;  // 0-weight variants + no anchor are both legal
    edges.fusion_variant_weight = 0.0f;
    edges.fusion_anchor_max = 0;
    EXPECT_TRUE(ValidateRagFusionConfig(edges));
}

// Exercise the public production assembly rather than mirroring its anonymous
// helpers. The fixture is fully offline: it uses a stub embedder, a real temporary
// namespace store, deterministic in-memory index hits, and a scripted LLM double.
class QueryWiringHttpCoverage : public ::testing::Test {
protected:
    static constexpr const char* kNamespace = "query-wiring-unit";
    static constexpr const char* kNoAuthUser = "anonymous";

    void SetUp() override {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        harness_ = std::make_unique<cortrix::doc_summary::test::DiscoveryPoolHarness>(
            std::filesystem::temp_directory_path() /
            ("query_wiring_http_" + std::to_string(stamp)));
        ASSERT_TRUE(harness_->Admit(kNamespace).ok());

        embedder_ = std::make_unique<cortrix::OnnxEmbedder>("", 128);
        ASSERT_TRUE(embedder_->Init().ok());
        fusion_ = std::make_unique<RRFFusion>(60);
        permission_ = std::make_unique<cortrix::tenant::PermissionService>(nullptr);

        ASSERT_NO_FATAL_FAILURE(SeedDocuments());
        ASSERT_NO_FATAL_FAILURE(SeedUserFacts());

        llm_ = std::make_shared<NiceMock<cortrix::llm::MockLlmClient>>();
        ON_CALL(*llm_, Chat(_, _))
            .WillByDefault(Invoke([](const std::string& prompt,
                                     const cortrix::llm::LlmCallConfig& config) {
                cortrix::llm::ChatCompletionResponse response;
                response.status = Status::Ok();
                response.model = config.model;
                response.finish_reason = "stop";
                if (prompt.find("degrade path") != std::string::npos) {
                    response.content = "{invalid-json";
                } else if (config.model == "unit-variant-model") {
                    response.content =
                        R"({"variants":[{"strategy":"paraphrase","query":"semantic memory"},{"strategy":"subquery","query":"agent context"}]})";
                } else {
                    response.content = R"({"ranking":[2,1]})";
                }
                response.content_length = static_cast<int>(response.content.size());
                return response;
            }));

        auth_.set_enabled(false);
        server_ = std::make_unique<httplib::Server>();
        wiring_ = std::make_unique<CrossNsQueryWiring>(
            harness_->ipool(), *embedder_, *fusion_, *permission_,
            /*sparse_registry=*/nullptr, llm_, /*engine_instr=*/nullptr,
            /*reranker_model_dir=*/"",
            /*query_complexity_model_dir=*/"/definitely/missing/query-complexity",
            /*candidate_multiplier=*/3, /*max_candidates=*/50,
            /*reranker_execution_provider=*/"cpu");
        ASSERT_TRUE(wiring_->IsReady());
        wiring_->Register(*server_, auth_);

        port_ = server_->bind_to_any_port("127.0.0.1");
        ASSERT_GT(port_, 0);
        server_thread_ = std::thread([this] { server_->listen_after_bind(); });

        httplib::Client client("127.0.0.1", port_);
        bool connected = false;
        for (int i = 0; i < 50; ++i) {
            if (client.Get("/__query_wiring_ready_probe")) {
                connected = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        ASSERT_TRUE(connected);
    }

    void TearDown() override {
        if (server_) server_->stop();
        if (server_thread_.joinable()) server_thread_.join();
    }

    void SeedDocuments() {
        const std::string metadata =
            json{{"status", "generated"},
                 {"keywords", json::array({"semantic", "memory"})},
                 {"topics", json::array({"agent context"})},
                 {"one_liner", "A deterministic query wiring fixture"}}
                .dump();
        const uint64_t first = harness_->SeedDocSummaryBlock(
            kNamespace, "doc-query-wiring-1", "Semantic memory stays near agents.",
            metadata);
        const uint64_t second = harness_->SeedDocSummaryBlock(
            kNamespace, "doc-query-wiring-2", "Agent context supports retrieval.",
            metadata);
        ASSERT_NE(harness_->fake_index(), nullptr);
        harness_->fake_index()->set_search_result({{first, 0.1f}, {second, 0.2f}});
    }

    void SeedUserFacts() {
        cortrix::resource::NamespaceFacade facade(harness_->ipool(), kNamespace);
        ASSERT_TRUE(facade.Acquire().ok());
        cortrix::memory::MemoryBlockAdapter adapter(facade.store(), nullptr, nullptr);
        for (int i = 1; i <= 3; ++i) {
            cortrix::memory::MemoryBlockRecord record;
            record.block_id = "01QUERYFACT" + std::to_string(i);
            record.user_id = kNoAuthUser;
            record.content = "query wiring fact " + std::to_string(i);
            record.metadata_json = {
                {"memory_type", i == 2 ? "preference" : "fact"},
                {"status", "active"},
                {"user_id", kNoAuthUser},
                {"created_at", "2026-07-21T00:00:0" + std::to_string(i) + "Z"},
                {"confidence", 0.8 + 0.01 * i},
            };
            ASSERT_TRUE(adapter.InsertMemoryBlock(record).ok());
        }
    }

    json BaseBody() const {
        return json{{"query", "semantic memory for agents"},
                    {"namespaces", json::array({kNamespace})},
                    {"top_k", 2}};
    }

    httplib::Result PostJson(const std::string& path, const json& body) const {
        httplib::Client client("127.0.0.1", port_);
        return client.Post(path, body.dump(), "application/json");
    }

    std::unique_ptr<cortrix::doc_summary::test::DiscoveryPoolHarness> harness_;
    std::unique_ptr<cortrix::OnnxEmbedder> embedder_;
    std::unique_ptr<RRFFusion> fusion_;
    std::unique_ptr<cortrix::tenant::PermissionService> permission_;
    std::shared_ptr<NiceMock<cortrix::llm::MockLlmClient>> llm_;
    ApiKeyAuth auth_;
    std::unique_ptr<httplib::Server> server_;
    std::unique_ptr<CrossNsQueryWiring> wiring_;
    std::thread server_thread_;
    int port_ = -1;
};

TEST_F(QueryWiringHttpCoverage, RequestMatrixExercisesProductionClosure) {
    auto expect_status = [this](const std::string& path, const json& body, int status,
                                const char* expected_code = nullptr) {
        auto response = PostJson(path, body);
        ASSERT_TRUE(response) << path;
        EXPECT_EQ(response->status, status) << path << "\n" << response->body;
        if (status >= 400) {
            const json error_body = json::parse(
                response->body, /*callback=*/nullptr, /*allow_exceptions=*/false);
            ASSERT_FALSE(error_body.is_discarded()) << path << "\n" << response->body;
            ASSERT_TRUE(error_body.contains("error")) << path << "\n" << response->body;
            ASSERT_TRUE(error_body["error"].contains("code"))
                << path << "\n" << response->body;
            ASSERT_TRUE(error_body["error"]["code"].is_string())
                << path << "\n" << response->body;
            const std::string actual_code =
                error_body["error"]["code"].get<std::string>();
            if (expected_code != nullptr) {
                EXPECT_EQ(actual_code, expected_code) << path << "\n" << response->body;
            } else {
                EXPECT_FALSE(actual_code.empty());
            }
        }
    };

    {
        httplib::Client client("127.0.0.1", port_);
        auto response = client.Post("/api/v1/query", "{not-json", "application/json");
        ASSERT_TRUE(response);
        EXPECT_EQ(response->status, 400) << response->body;
        const json error_body = json::parse(response->body);
        ASSERT_TRUE(error_body["error"].contains("code"));
        ASSERT_TRUE(error_body["error"]["code"].is_string());
        EXPECT_EQ(error_body["error"]["code"], "INVALID_ARGUMENT");
    }

    json invalid_granularity = BaseBody();
    invalid_granularity["granularity"] = "invalid";
    expect_status("/api/v1/query", invalid_granularity, 400, "INVALID_ARGUMENT");
    expect_status("/api/v1/query?granularity=invalid", BaseBody(), 400,
                  "INVALID_ARGUMENT");

    json invalid_crag = BaseBody();
    invalid_crag["crag"] = "yes";
    expect_status("/api/v1/query", invalid_crag, 400, "INVALID_ARGUMENT");
    expect_status("/api/v1/query?crag=maybe", BaseBody(), 400,
                  "INVALID_ARGUMENT");

    json invalid_route = BaseBody();
    invalid_route["route"] = "invalid";
    expect_status("/api/v1/query", invalid_route, 400,
                  "CX_ERR_F39_FORCE_ROUTE_INVALID");
    expect_status("/api/v1/query?route=invalid", BaseBody(), 400,
                  "CX_ERR_F39_FORCE_ROUTE_INVALID");

    json chat = BaseBody();
    chat["query"] = "hi";
    chat["route"] = "chat";
    chat["namespaces"] = json::array({kNamespace, "missing-query-wiring-ns"});
    auto chat_response = PostJson("/api/v1/query", chat);
    ASSERT_TRUE(chat_response);
    ASSERT_EQ(chat_response->status, 200) << chat_response->body;
    const json chat_json = json::parse(chat_response->body);
    ASSERT_EQ(chat_json["results"].size(), 2u);
    EXPECT_EQ(chat_json["meta"]["via_path"], "chat_memory_only");
    EXPECT_EQ(chat_json["results"][0]["content"], "query wiring fact 3");

    json rich = BaseBody();
    rich["query"] = "compare semantic memory and agent context in detail";
    rich["route"] = "complex";
    rich["granularity"] = "both";
    rich["rerank"] = true;
    rich["crag"] = true;
    rich["explain"] = true;
    rich["locale"] = "en";
    rich["search_config"] = {{"enable_vector", true},
                              {"enable_bm25", true},
                              {"enable_sparse", true}};
    rich["filter"] = {{"source", "unit"}, {"ignored_non_string", 7}};
    rich["rag_fusion"] = true;
    rich["rag_fusion_config"] = {
        {"enabled", true},
        {"variant_count", 2},
        {"rrf_k", 60},
        {"timeout_ms", 5000},
        {"locale", "en"},
        {"model", "unit-variant-model"},
        {"candidate_multiplier", 2},
        {"max_candidates", 20},
        {"final_rerank", false},
        {"activation_policy", "always"},
        {"activation_score_margin", 0.1},
        {"activation_min_results", 2},
        {"fusion_original_weight", 1.7},
        {"fusion_variant_weight", 0.5},
        {"fusion_anchor_max", 2},
    };
    rich["llm_rerank"] = true;
    rich["llm_rerank_config"] = {
        {"enabled", true},
        {"top_n", 2},
        {"max_doc_chars", 200},
        {"timeout_ms", 5000},
        {"model", "unit-rerank-model"},
        {"locale", "en"},
        {"consensus_runs", 1},
    };

    const std::string rich_params =
        "/api/v1/query?route=complex&granularity=both&explain=1&crag=true"
        "&rag_fusion=1&locale=zh&rag_fusion_candidate_multiplier=2"
        "&rag_fusion_max_candidates=20&rag_fusion_final_rerank=false"
        "&rag_fusion_activation_policy=always&rag_fusion_activation_score_margin=0.1"
        "&rag_fusion_activation_min_results=2&llm_rerank=1&llm_rerank_top_n=2"
        "&llm_rerank_model=unit-param-model&llm_rerank_timeout_ms=5000"
        "&llm_rerank_consensus_runs=1";
    auto rich_response = PostJson(rich_params, rich);
    ASSERT_TRUE(rich_response);
    ASSERT_EQ(rich_response->status, 200) << rich_response->body;
    const json rich_json = json::parse(rich_response->body);
    ASSERT_TRUE(rich_json.contains("results"));
    ASSERT_FALSE(rich_json["results"].empty()) << rich_response->body;
    EXPECT_LE(rich_json["results"].size(), 2u) << rich_response->body;
    ASSERT_TRUE(rich_json.contains("explain"));
    EXPECT_EQ(rich_json["explain"]["granularity"], "both");
    const auto& rich_features = rich_json["explain"]["llm_dependent_features"];
    const auto& rich_rag_fusion = rich_features["rag_fusion"];
    const auto& rich_llm_rerank = rich_features["llm_rerank"];
    EXPECT_TRUE(rich_rag_fusion["active"].get<bool>());
    EXPECT_FALSE(rich_rag_fusion["degraded"].get<bool>());
    EXPECT_EQ(rich_rag_fusion["variant_count"], 3);
    EXPECT_TRUE(rich_llm_rerank["active"].get<bool>());
    EXPECT_FALSE(rich_llm_rerank["degraded"].get<bool>());
    EXPECT_EQ(rich_llm_rerank["votes_ok"], 1);

    json degraded = rich;
    degraded["query"] = "degrade path";
    auto degraded_response = PostJson(rich_params, degraded);
    ASSERT_TRUE(degraded_response);
    ASSERT_EQ(degraded_response->status, 200) << degraded_response->body;
    const json degraded_json = json::parse(degraded_response->body);
    ASSERT_TRUE(degraded_json.contains("explain"));
    const auto& degraded_features =
        degraded_json["explain"]["llm_dependent_features"];
    const auto& degraded_rag_fusion = degraded_features["rag_fusion"];
    const auto& degraded_llm_rerank = degraded_features["llm_rerank"];
    EXPECT_TRUE(degraded_rag_fusion["active"].get<bool>());
    EXPECT_TRUE(degraded_rag_fusion["degraded"].get<bool>());
    ASSERT_TRUE(degraded_rag_fusion.contains("degrade_reason"));
    EXPECT_FALSE(degraded_rag_fusion["degrade_reason"].get<std::string>().empty());
    EXPECT_TRUE(degraded_llm_rerank["active"].get<bool>());
    EXPECT_TRUE(degraded_llm_rerank["degraded"].get<bool>());
    ASSERT_TRUE(degraded_llm_rerank.contains("degrade_reason"));
    EXPECT_FALSE(degraded_llm_rerank["degrade_reason"].get<std::string>().empty());

    json invalid_rag = BaseBody();
    invalid_rag["route"] = "complex";
    invalid_rag["rag_fusion_config"] = {{"variant_count", 99}};
    expect_status("/api/v1/query", invalid_rag, 400,
                  "CX_ERR_RAG_FUSION_CONFIG_INVALID");

    json invalid_llm = BaseBody();
    invalid_llm["route"] = "complex";
    invalid_llm["llm_rerank_config"] = {{"top_n", 1}};
    expect_status("/api/v1/query", invalid_llm, 400,
                  "CX_ERR_LLM_RERANK_CONFIG_INVALID");

    const std::string bad_numeric_params =
        "/api/v1/query?route=complex&granularity=both&crag=true&explain=1"
        "&rag_fusion=1&llm_rerank=1"
        "&rag_fusion_candidate_multiplier=bad&rag_fusion_max_candidates=bad"
        "&rag_fusion_activation_score_margin=bad"
        "&rag_fusion_activation_min_results=bad&llm_rerank_top_n=bad"
        "&llm_rerank_timeout_ms=bad&llm_rerank_consensus_runs=bad";
    auto bad_numeric_response = PostJson(bad_numeric_params, rich);
    ASSERT_TRUE(bad_numeric_response);
    ASSERT_EQ(bad_numeric_response->status, 200) << bad_numeric_response->body;
    const json bad_numeric_json = json::parse(bad_numeric_response->body);
    ASSERT_TRUE(bad_numeric_json.contains("explain"));
    const auto& bad_numeric_features =
        bad_numeric_json["explain"]["llm_dependent_features"];
    const auto& bad_numeric_rag = bad_numeric_features["rag_fusion"];
    const auto& bad_numeric_llm = bad_numeric_features["llm_rerank"];
    EXPECT_TRUE(bad_numeric_rag["active"].get<bool>());
    EXPECT_FALSE(bad_numeric_rag["degraded"].get<bool>());
    EXPECT_EQ(bad_numeric_rag["variant_count"], 3);
    EXPECT_TRUE(bad_numeric_llm["active"].get<bool>());
    EXPECT_FALSE(bad_numeric_llm["degraded"].get<bool>());
    EXPECT_EQ(bad_numeric_llm["model"], "unit-rerank-model");
    EXPECT_EQ(bad_numeric_llm["votes_ok"], 1);

    json body_precedence = BaseBody();
    body_precedence["route"] = "invalid";
    body_precedence["granularity"] = "invalid";
    body_precedence["crag"] = true;
    auto precedence_response = PostJson(
        "/api/v1/query?route=complex&granularity=doc&crag=0&explain=1",
        body_precedence);
    ASSERT_TRUE(precedence_response);
    ASSERT_EQ(precedence_response->status, 200) << precedence_response->body;
    const json precedence_json = json::parse(precedence_response->body);
    ASSERT_TRUE(precedence_json.contains("explain"));
    EXPECT_EQ(precedence_json["explain"]["routing_path"], "complex");
    EXPECT_EQ(precedence_json["explain"]["granularity"], "doc");
    EXPECT_EQ(precedence_json["explain"]["crag_verdict"], "");
    EXPECT_FALSE(precedence_json["meta"].contains("crag_verdict"));

    json deprecated = BaseBody();
    deprecated["namespace"] = kNamespace;
    expect_status("/api/v1/query", deprecated, 400, "CX_ERR_DEPRECATED_FIELD");

    json too_many = BaseBody();
    too_many["route"] = "complex";
    too_many["namespaces"] = json::array();
    for (int i = 0; i < 101; ++i) {
        too_many["namespaces"].push_back("query-wiring-ns-" + std::to_string(i));
    }
    auto too_many_response = PostJson("/api/v1/query", too_many);
    ASSERT_TRUE(too_many_response);
    ASSERT_EQ(too_many_response->status, 400) << too_many_response->body;
    const json too_many_json = json::parse(too_many_response->body);
    EXPECT_EQ(too_many_json["error"]["code"], "CX_ERR_TOO_MANY_NAMESPACES");

    json wildcard = BaseBody();
    wildcard["route"] = "simple";
    wildcard["namespaces"] = json::array({"*"});
    expect_status("/api/v1/query", wildcard, 200);

    json missing_query = {{"namespaces", json::array({kNamespace})}};
    expect_status("/api/v1/query", missing_query, 400, "CX_ERR_BAD_REQUEST");
}

// The production gate (use_rag_fusion = complex && enabled && llm_available)
// stays OFF on its first two legs, asserted through the live closure via
// ?explain: (a) simple route + opt-in → inactive; (b) complex route without
// opt-in (config default disabled) → inactive. The gate-ON case is pinned by
// RequestMatrixExercisesProductionClosure above (rag_fusion.active=true).
TEST_F(QueryWiringHttpCoverage, RagFusionGateStaysOffOnSimpleRouteAndWithoutOptIn) {
    // (a) simple route, rag_fusion opted in → gate off (F36 only on Complex).
    json simple = BaseBody();
    simple["route"] = "simple";
    simple["explain"] = true;
    simple["rag_fusion"] = true;
    auto simple_res = PostJson("/api/v1/query", simple);
    ASSERT_TRUE(simple_res);
    ASSERT_EQ(simple_res->status, 200) << simple_res->body;
    const json simple_json = json::parse(simple_res->body);
    EXPECT_EQ(simple_json["explain"]["routing_path"], "simple");
    // Gate off → the route never runs the rag-fusion stage, so the explain
    // carries NO rag_fusion feature block (the block appears only when
    // use_rag_fusion is true — the serializer-contract decision on the
    // inactive-state tests).
    EXPECT_FALSE(simple_json["explain"].contains("llm_dependent_features"))
        << simple_res->body;

    // (b) complex route, no opt-in → default-disabled config keeps the gate off.
    json complex_no_optin = BaseBody();
    complex_no_optin["route"] = "complex";
    complex_no_optin["explain"] = true;
    auto complex_res = PostJson("/api/v1/query", complex_no_optin);
    ASSERT_TRUE(complex_res);
    ASSERT_EQ(complex_res->status, 200) << complex_res->body;
    const json complex_json = json::parse(complex_res->body);
    EXPECT_EQ(complex_json["explain"]["routing_path"], "complex");
    EXPECT_FALSE(complex_json["explain"].contains("llm_dependent_features"))
        << complex_res->body;
}

// Third gate leg ([R7]): rag_fusion opted in on the complex route but the
// wiring has NO LLM client → the gate stays off and the request degrades to
// plain scatter (200) instead of crashing on a null LLM. Uses a second wiring
// built without an LLM on its own port.
TEST_F(QueryWiringHttpCoverage, RagFusionGateOffWithoutLlm) {
    httplib::Server no_llm_server;
    CrossNsQueryWiring no_llm_wiring(
        harness_->ipool(), *embedder_, *fusion_, *permission_,
        /*sparse_registry=*/nullptr, /*llm=*/nullptr, /*engine_instr=*/nullptr,
        /*reranker_model_dir=*/"",
        /*query_complexity_model_dir=*/"/definitely/missing/query-complexity",
        /*candidate_multiplier=*/3, /*max_candidates=*/50,
        /*reranker_execution_provider=*/"cpu");
    ASSERT_TRUE(no_llm_wiring.IsReady());
    no_llm_wiring.Register(no_llm_server, auth_);
    const int port = no_llm_server.bind_to_any_port("127.0.0.1");
    ASSERT_GT(port, 0);
    std::thread listen_thread([&] { no_llm_server.listen_after_bind(); });
    for (int i = 0; i < 50 && !no_llm_server.is_running(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    json body = BaseBody();
    body["route"] = "complex";
    body["explain"] = true;
    body["rag_fusion"] = true;
    httplib::Client client("127.0.0.1", port);
    auto res = client.Post("/api/v1/query", body.dump(), "application/json");

    no_llm_server.stop();
    listen_thread.join();

    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 200) << res->body;
    const json j = json::parse(res->body);
    EXPECT_EQ(j["explain"]["routing_path"], "complex");
    // Gate off (no LLM) → plain scatter ran; no rag_fusion feature block.
    EXPECT_FALSE(j["explain"].contains("llm_dependent_features"))
        << "no-LLM wiring must keep the rag-fusion gate off (degrade to scatter): "
        << res->body;
}

}  // namespace
}  // namespace cortrix::query
