#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include <memory>
#include <string>
#include <vector>

#include "cortrix/common/in_memory_global_config.h"
#include "cortrix/spc/contextual_enricher.h"
#include "cortrix/spc/contextual_metrics.h"
#include "mock_llm_client.h"

// F35 — ContextualRetrievalEnricher: ISpcEnricher impl (Enrich + EnrichBatch +
// IsAvailable + Name), GenerateContextualizedText (LLM mock), BuildPrompt
// (default Anthropic + NS-override template), the three-layer config resolver,
// and the §7 L1/L2/L3 fallback paths. Enrich() populates the 3 F35 EnrichResult
// fields (contextualized_text / contextualized_embedding / contextualized_status).
namespace cortrix::spc {
namespace {

using ::testing::_;
using ::testing::Return;

llm::ChatCompletionResponse OkChat(std::string content) {
    llm::ChatCompletionResponse r;
    r.content = std::move(content);
    r.model = "gpt-4o-mini";
    r.prompt_tokens = 30;
    r.completion_tokens = 20;
    return r;  // status defaults ok()
}

DocumentMetadata DocMeta(std::string title = "Q3 2026 Financial Report") {
    DocumentMetadata m;
    m.doc_title = std::move(title);
    return m;
}

ChunkContext MakeCtx(std::string chunk, std::string prev = "", std::string next = "") {
    ChunkContext c;
    c.chunk_text = std::move(chunk);
    c.prev_chunk_text = std::move(prev);
    c.next_chunk_text = std::move(next);
    return c;
}

/// Test double for the IContextualEmbedder seam — deterministic vectors, or a
/// configurable failure (to exercise the embedding-degrade path).
class FakeContextualEmbedder : public IContextualEmbedder {
public:
    explicit FakeContextualEmbedder(int dim = 1024) : dim_(dim) {}

    Result<std::vector<float>> Embed(const std::string& text) override {
        ++calls;
        last_text = text;
        if (fail) return Status::Unavailable("fake embed fail");
        std::vector<float> v(dim_, 0.0f);
        float seed = 0.0f;
        for (char c : text) seed += static_cast<float>(c);
        if (dim_ > 0) v[0] = seed;
        return v;
    }

    bool fail = false;
    int calls = 0;
    std::string last_text;

private:
    int dim_;
};

std::shared_ptr<FakeContextualEmbedder> MakeEmbedder() {
    return std::make_shared<FakeContextualEmbedder>(8);
}

// ---------- ISpcEnricher contract ----------

TEST(F35ContextualEnricherTest, UsableViaBasePointer) {
    ContextualRetrievalEnricher e(ContextualRetrievalConfig{},
                                  std::make_shared<llm::MockLlmClient>(), MakeEmbedder());
    ISpcEnricher* base = &e;  // polymorphic use through the frozen interface
    EXPECT_EQ(base->Name(), "f35_contextual_retrieval");
}

TEST(F35ContextualEnricherTest, NameIsF35) {
    ContextualRetrievalEnricher e(ContextualRetrievalConfig{},
                                  std::make_shared<llm::MockLlmClient>(), MakeEmbedder());
    EXPECT_EQ(e.Name(), "f35_contextual_retrieval");
}

TEST(F35ContextualEnricherTest, AvailableWithClientAndEnabled) {
    ContextualRetrievalEnricher e(ContextualRetrievalConfig{},
                                  std::make_shared<llm::MockLlmClient>(), MakeEmbedder());
    EXPECT_TRUE(e.IsAvailable());
}

TEST(F35ContextualEnricherTest, UnavailableWithoutClient) {
    ContextualRetrievalEnricher e(ContextualRetrievalConfig{}, nullptr, MakeEmbedder());
    EXPECT_FALSE(e.IsAvailable());
}

TEST(F35ContextualEnricherTest, UnavailableWhenDisabled) {
    ContextualRetrievalConfig cfg;
    cfg.enabled = false;
    ContextualRetrievalEnricher e(cfg, std::make_shared<llm::MockLlmClient>(),
                                  MakeEmbedder());
    EXPECT_FALSE(e.IsAvailable());
}

// ---------- GenerateContextualizedText ----------

TEST(F35ContextualEnricherTest, GeneratesPrefixText) {
    auto llm = std::make_shared<llm::MockLlmClient>();
    EXPECT_CALL(*llm, Chat(_, _))
        .WillOnce(Return(OkChat("This chunk is from the Q3 financial report.")));
    ContextualRetrievalEnricher e(ContextualRetrievalConfig{}, llm, MakeEmbedder());

    auto r = e.GenerateContextualizedText("Revenue 520M, +23% YoY", DocMeta(),
                                          MakeCtx("Revenue 520M, +23% YoY"));
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_EQ(r.value(), "This chunk is from the Q3 financial report.");
}

TEST(F35ContextualEnricherTest, GenerateNullClientIsStartupNoLlm) {
    ContextualRetrievalEnricher e(ContextualRetrievalConfig{}, nullptr, MakeEmbedder());
    auto r = e.GenerateContextualizedText("chunk", DocMeta(), MakeCtx("chunk"));
    EXPECT_FALSE(r.ok());
    EXPECT_NE(r.status().message().find("CX_ERR_F35_STARTUP_NO_LLM"),
              std::string::npos);
}

TEST(F35ContextualEnricherTest, GenerateLlmFailureIsLlmFailed) {
    auto llm = std::make_shared<llm::MockLlmClient>();
    llm::ChatCompletionResponse fail;
    fail.status = Status::Unavailable("connect refused");
    EXPECT_CALL(*llm, Chat(_, _)).WillOnce(Return(fail));
    ContextualRetrievalEnricher e(ContextualRetrievalConfig{}, llm, MakeEmbedder());

    auto r = e.GenerateContextualizedText("chunk", DocMeta(), MakeCtx("chunk"));
    EXPECT_FALSE(r.ok());
    EXPECT_NE(r.status().message().find("CX_ERR_F35_LLM_FAILED"), std::string::npos);
}

TEST(F35ContextualEnricherTest, GenerateOverlongOutputIsPromptInjection) {
    auto llm = std::make_shared<llm::MockLlmClient>();
    // max_output_tokens default 80 → guard = 160 chars. Return 200 chars.
    EXPECT_CALL(*llm, Chat(_, _))
        .WillOnce(Return(OkChat(std::string(200, 'x'))));
    ContextualRetrievalEnricher e(ContextualRetrievalConfig{}, llm, MakeEmbedder());

    auto r = e.GenerateContextualizedText("chunk", DocMeta(), MakeCtx("chunk"));
    EXPECT_FALSE(r.ok());
    EXPECT_NE(r.status().message().find("CX_ERR_F35_PROMPT_INJECTION"),
              std::string::npos);
}

TEST(F35ContextualEnricherTest, GeneratePassesCallConfig) {
    auto llm = std::make_shared<llm::MockLlmClient>();
    llm::LlmCallConfig captured;
    EXPECT_CALL(*llm, Chat(_, _))
        .WillOnce([&](const std::string&, const llm::LlmCallConfig& c) {
            captured = c;
            return OkChat("ctx");
        });
    ContextualRetrievalConfig cfg;
    cfg.llm_model = "gpt-4o";
    cfg.max_output_tokens = 120;
    ContextualRetrievalEnricher e(cfg, llm, MakeEmbedder());
    (void)e.GenerateContextualizedText("chunk", DocMeta(), MakeCtx("chunk"));

    EXPECT_EQ(captured.model, "gpt-4o");
    EXPECT_EQ(captured.max_tokens, 120);
    EXPECT_DOUBLE_EQ(captured.temperature, 0.0);
    EXPECT_EQ(captured.timeout_ms, kContextualTimeoutMs);
}

// ---------- BuildPrompt (default Anthropic + NS override) ----------

TEST(F35ContextualEnricherTest, DefaultPromptHasAnthropicStructure) {
    ContextualRetrievalEnricher e(ContextualRetrievalConfig{},
                                  std::make_shared<llm::MockLlmClient>(), MakeEmbedder());
    std::string p = e.BuildPrompt("THE CHUNK BODY", DocMeta("My Report"),
                                  MakeCtx("THE CHUNK BODY", "PREVCTX", "NEXTCTX"));
    EXPECT_NE(p.find("<document_metadata>"), std::string::npos);
    EXPECT_NE(p.find("title: My Report"), std::string::npos);
    EXPECT_NE(p.find("<previous_chunk>PREVCTX</previous_chunk>"), std::string::npos);
    EXPECT_NE(p.find("<chunk>THE CHUNK BODY</chunk>"), std::string::npos);
    EXPECT_NE(p.find("<next_chunk>NEXTCTX</next_chunk>"), std::string::npos);
    EXPECT_NE(p.find("succinct context"), std::string::npos);
}

TEST(F35ContextualEnricherTest, DefaultPromptEmptyBoundariesRenderEmpty) {
    ContextualRetrievalEnricher e(ContextualRetrievalConfig{},
                                  std::make_shared<llm::MockLlmClient>(), MakeEmbedder());
    std::string p = e.BuildPrompt("body", DocMeta("R"), MakeCtx("body"));
    // doc start/end → empty prev/next chunk tags (not the literal placeholders).
    EXPECT_NE(p.find("<previous_chunk></previous_chunk>"), std::string::npos);
    EXPECT_NE(p.find("<next_chunk></next_chunk>"), std::string::npos);
}

TEST(F35ContextualEnricherTest, NsOverrideTemplateSubstitutesPlaceholders) {
    ContextualRetrievalConfig cfg;
    cfg.prompt_template =
        "T={{doc_title}} P={{prev_chunk_text}} C={{chunk_text}} N={{next_chunk_text}}";
    ContextualRetrievalEnricher e(cfg, std::make_shared<llm::MockLlmClient>(),
                                  MakeEmbedder());
    std::string p = e.BuildPrompt("CC", DocMeta("TT"), MakeCtx("CC", "PP", "NN"));
    EXPECT_EQ(p, "T=TT P=PP C=CC N=NN");
}

// ---------- Enrich: L3 success path (status=1 generated) ----------

TEST(F35ContextualEnricherTest, EnrichSuccessPopulatesAllThreeFields) {
    auto llm = std::make_shared<llm::MockLlmClient>();
    EXPECT_CALL(*llm, Chat(_, _)).WillOnce(Return(OkChat("PREFIX CONTEXT")));
    auto emb = MakeEmbedder();
    ContextualRetrievalEnricher e(ContextualRetrievalConfig{}, llm, emb);

    EnrichResult res = e.Enrich("chunk body", DocMeta(), MakeCtx("chunk body"));
    EXPECT_TRUE(res.ok());                              // status 0 (Block still written)
    EXPECT_EQ(res.enricher_name, "f35_contextual_retrieval");
    EXPECT_EQ(res.contextualized_status, 1);            // generated
    ASSERT_TRUE(res.contextualized_text.has_value());
    // contextualized_text = prefix + "\n" + original chunk (§1.1).
    EXPECT_EQ(*res.contextualized_text, "PREFIX CONTEXT\nchunk body");
    ASSERT_TRUE(res.contextualized_embedding.has_value());
    EXPECT_EQ(res.contextualized_embedding->size(), 8u);
    // The embedder embedded the contextualized text (not the raw chunk).
    EXPECT_EQ(emb->last_text, "PREFIX CONTEXT\nchunk body");
    EXPECT_TRUE(res.entities.empty());                  // F35 does no NER
    EXPECT_TRUE(res.summary.empty());
}

// ---------- Enrich: L2 skip path (status=3 skipped_no_llm) ----------

TEST(F35ContextualEnricherTest, EnrichNullClientSkipsNoLlm) {
    ContextualRetrievalEnricher e(ContextualRetrievalConfig{}, nullptr, MakeEmbedder());
    EnrichResult res = e.Enrich("chunk", DocMeta(), MakeCtx("chunk"));
    EXPECT_TRUE(res.ok());
    EXPECT_EQ(res.contextualized_status, 3);            // skipped_no_llm
    EXPECT_FALSE(res.contextualized_text.has_value());
    EXPECT_FALSE(res.contextualized_embedding.has_value());
}

TEST(F35ContextualEnricherTest, EnrichDisabledSkipsNoLlm) {
    ContextualRetrievalConfig cfg;
    cfg.enabled = false;
    ContextualRetrievalEnricher e(cfg, std::make_shared<llm::MockLlmClient>(),
                                  MakeEmbedder());
    EnrichResult res = e.Enrich("chunk", DocMeta(), MakeCtx("chunk"));
    EXPECT_EQ(res.contextualized_status, 3);
}

// ---------- Enrich: L3 degrade paths (status=2 failed) ----------

TEST(F35ContextualEnricherTest, EnrichLlmFailureDegradesToFailed) {
    auto llm = std::make_shared<llm::MockLlmClient>();
    llm::ChatCompletionResponse fail;
    fail.status = Status::Unavailable("boom");
    EXPECT_CALL(*llm, Chat(_, _)).WillOnce(Return(fail));
    ContextualRetrievalEnricher e(ContextualRetrievalConfig{}, llm, MakeEmbedder());

    EnrichResult res = e.Enrich("chunk", DocMeta(), MakeCtx("chunk"));
    EXPECT_TRUE(res.ok());                              // Block still written (degrade)
    EXPECT_EQ(res.contextualized_status, 2);            // failed
    EXPECT_FALSE(res.contextualized_text.has_value());
    EXPECT_FALSE(res.contextualized_embedding.has_value());
    EXPECT_NE(res.error_msg.find("CX_ERR_F35_LLM_FAILED"), std::string::npos);
}

TEST(F35ContextualEnricherTest, EnrichNullEmbedderDegradesButKeepsText) {
    auto llm = std::make_shared<llm::MockLlmClient>();
    EXPECT_CALL(*llm, Chat(_, _)).WillOnce(Return(OkChat("PFX")));
    ContextualRetrievalEnricher e(ContextualRetrievalConfig{}, llm, /*embedder=*/nullptr);

    EnrichResult res = e.Enrich("chunk", DocMeta(), MakeCtx("chunk"));
    EXPECT_EQ(res.contextualized_status, 2);            // failed (embedding half)
    ASSERT_TRUE(res.contextualized_text.has_value());   // text still produced (partial)
    EXPECT_EQ(*res.contextualized_text, "PFX\nchunk");
    EXPECT_FALSE(res.contextualized_embedding.has_value());
    EXPECT_NE(res.error_msg.find("CX_ERR_F35_EMBEDDING_FAILED"), std::string::npos);
}

TEST(F35ContextualEnricherTest, EnrichEmbedderFailureDegradesToFailed) {
    auto llm = std::make_shared<llm::MockLlmClient>();
    EXPECT_CALL(*llm, Chat(_, _)).WillOnce(Return(OkChat("PFX")));
    auto emb = MakeEmbedder();
    emb->fail = true;
    ContextualRetrievalEnricher e(ContextualRetrievalConfig{}, llm, emb);

    EnrichResult res = e.Enrich("chunk", DocMeta(), MakeCtx("chunk"));
    EXPECT_EQ(res.contextualized_status, 2);            // failed
    EXPECT_FALSE(res.contextualized_embedding.has_value());
    EXPECT_NE(res.error_msg.find("CX_ERR_F35_EMBEDDING_FAILED"), std::string::npos);
}

// ---------- Enrich: metrics side effects ----------

TEST(F35ContextualEnricherTest, EnrichRecordsChunkMetric) {
    ContextualRetrievalMetrics::Instance().ResetForTest();
    auto llm = std::make_shared<llm::MockLlmClient>();
    EXPECT_CALL(*llm, Chat(_, _)).WillOnce(Return(OkChat("PFX")));
    ContextualRetrievalEnricher e(ContextualRetrievalConfig{}, llm, MakeEmbedder());
    (void)e.Enrich("chunk", DocMeta(), MakeCtx("chunk"));
    EXPECT_EQ(ContextualRetrievalMetrics::Instance().ChunkCount(
                  ContextualRetrievalMetrics::ChunkStatus::kGenerated),
              1u);
    ContextualRetrievalMetrics::Instance().ResetForTest();
}

// ---------- EnrichBatch ----------

TEST(F35ContextualEnricherTest, EnrichBatchRunsPerContext) {
    auto llm = std::make_shared<llm::MockLlmClient>();
    EXPECT_CALL(*llm, Chat(_, _)).WillRepeatedly(Return(OkChat("PFX")));
    ContextualRetrievalEnricher e(ContextualRetrievalConfig{}, llm, MakeEmbedder());

    DocumentMetadata meta = DocMeta();
    std::vector<ChunkContext> ctxs;
    for (int i = 0; i < 3; ++i) {
        ChunkContext c = MakeCtx("chunk " + std::to_string(i));
        c.chunk_index = i;
        c.doc_metadata = &meta;
        ctxs.push_back(c);
    }
    auto results = e.EnrichBatch(ctxs);
    ASSERT_EQ(results.size(), 3u);
    for (const auto& r : results) {
        EXPECT_EQ(r.contextualized_status, 1);
        EXPECT_TRUE(r.contextualized_embedding.has_value());
    }
}

TEST(F35ContextualEnricherTest, EnrichBatchNullDocMetadataUsesEmpty) {
    auto llm = std::make_shared<llm::MockLlmClient>();
    EXPECT_CALL(*llm, Chat(_, _)).WillRepeatedly(Return(OkChat("PFX")));
    ContextualRetrievalEnricher e(ContextualRetrievalConfig{}, llm, MakeEmbedder());
    std::vector<ChunkContext> ctxs{MakeCtx("c0")};  // doc_metadata stays nullptr
    auto results = e.EnrichBatch(ctxs);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].contextualized_status, 1);
}

// ---------- Config resolution: three-layer override (§6.2) ----------

TEST(F35ContextualConfigTest, ResolveNullGlobalGivesDefaults) {
    ContextualRetrievalConfig cfg = ResolveContextualConfig(nullptr);
    EXPECT_TRUE(cfg.enabled);
    EXPECT_TRUE(cfg.prompt_template.empty());
    EXPECT_EQ(cfg.max_output_tokens, kContextualMaxOutputTokensDefault);
    EXPECT_EQ(cfg.llm_model, kContextualDefaultLlmModel);
}

TEST(F35ContextualConfigTest, ResolveReadsGlobalLayer) {
    InMemoryGlobalConfig gc;
    gc.Set(kContextualEnabledKey, "false");
    gc.Set(kContextualPromptTemplateKey, "MY TEMPLATE");
    gc.Set(kContextualMaxOutputTokensKey, "120");
    gc.Set(kContextualLlmModelKey, "gpt-4o");
    ContextualRetrievalConfig cfg = ResolveContextualConfig(&gc);
    EXPECT_FALSE(cfg.enabled);
    EXPECT_EQ(cfg.prompt_template, "MY TEMPLATE");
    EXPECT_EQ(cfg.max_output_tokens, 120);
    EXPECT_EQ(cfg.llm_model, "gpt-4o");
}

TEST(F35ContextualConfigTest, ResolveClampsMaxOutputTokens) {
    InMemoryGlobalConfig gc;
    gc.Set(kContextualMaxOutputTokensKey, "5000");  // above max 200
    EXPECT_EQ(ResolveContextualConfig(&gc).max_output_tokens,
              kContextualMaxOutputTokensMax);
    gc.Set(kContextualMaxOutputTokensKey, "1");      // below min 40
    EXPECT_EQ(ResolveContextualConfig(&gc).max_output_tokens,
              kContextualMaxOutputTokensMin);
}

TEST(F35ContextualConfigTest, ResolveUnparseableValueKeepsDefault) {
    InMemoryGlobalConfig gc;
    gc.Set(kContextualMaxOutputTokensKey, "not-a-number");  // GetInt fails → default
    EXPECT_EQ(ResolveContextualConfig(&gc).max_output_tokens,
              kContextualMaxOutputTokensDefault);
}

TEST(F35ContextualConfigTest, ResolveReadsTimeoutAndFloors) {
    InMemoryGlobalConfig gc;
    gc.Set(kContextualTimeoutMsKey, "60000");
    EXPECT_EQ(ResolveContextualConfig(&gc).timeout_ms, 60000);
    gc.Set(kContextualTimeoutMsKey, "0");  // nonsense → floored, never a 0ms deadline
    EXPECT_EQ(ResolveContextualConfig(&gc).timeout_ms, kContextualTimeoutMsMin);
}

TEST(F35ContextualConfigTest, ResolveTimeoutAbsentKeepsFrozenDefault) {
    InMemoryGlobalConfig gc;
    EXPECT_EQ(ResolveContextualConfig(&gc).timeout_ms, kContextualTimeoutMs);
}

TEST(F35ContextualEnricherTest, GeneratePassesConfiguredTimeout) {
    auto llm = std::make_shared<llm::MockLlmClient>();
    llm::LlmCallConfig captured;
    EXPECT_CALL(*llm, Chat(_, _))
        .WillOnce([&](const std::string&, const llm::LlmCallConfig& c) {
            captured = c;
            return OkChat("ctx");
        });
    ContextualRetrievalConfig cfg;
    cfg.timeout_ms = 60000;
    ContextualRetrievalEnricher e(cfg, llm, MakeEmbedder());
    (void)e.GenerateContextualizedText("chunk", DocMeta(), MakeCtx("chunk"));
    EXPECT_EQ(captured.timeout_ms, 60000);
}

TEST(F35ContextualConfigTest, ResolveMissingKeysKeepDefaults) {
    InMemoryGlobalConfig gc;  // nothing set
    ContextualRetrievalConfig cfg = ResolveContextualConfig(&gc);
    EXPECT_TRUE(cfg.enabled);
    EXPECT_EQ(cfg.max_output_tokens, kContextualMaxOutputTokensDefault);
    EXPECT_EQ(cfg.llm_model, kContextualDefaultLlmModel);
}

TEST(F35ContextualConfigTest, MergeNsOverrideEmptyJsonIsNoOp) {
    ContextualRetrievalConfig base;
    base.max_output_tokens = 90;
    ContextualRetrievalConfig merged = MergeNsOverride(base, "");
    EXPECT_EQ(merged.max_output_tokens, 90);
}

TEST(F35ContextualConfigTest, MergeNsOverrideFlatKeys) {
    ContextualRetrievalConfig base;
    ContextualRetrievalConfig merged = MergeNsOverride(
        base,
        R"({"prompt_template":"NS T","max_output_tokens":150,"llm_model":"gpt-4o","enabled":false})");
    EXPECT_EQ(merged.prompt_template, "NS T");
    EXPECT_EQ(merged.max_output_tokens, 150);
    EXPECT_EQ(merged.llm_model, "gpt-4o");
    EXPECT_FALSE(merged.enabled);
}

TEST(F35ContextualConfigTest, MergeNsOverrideNestedUnderContextualRetrieval) {
    ContextualRetrievalConfig base;
    ContextualRetrievalConfig merged = MergeNsOverride(
        base, R"({"contextual_retrieval":{"max_output_tokens":160,"llm_model":"gpt-4o"}})");
    EXPECT_EQ(merged.max_output_tokens, 160);
    EXPECT_EQ(merged.llm_model, "gpt-4o");
}

TEST(F35ContextualConfigTest, MergeNsOverrideClampsAndIgnoresMalformed) {
    ContextualRetrievalConfig base;
    base.max_output_tokens = 80;
    // max_output_tokens above range → clamp; llm_model wrong type → ignored.
    ContextualRetrievalConfig merged =
        MergeNsOverride(base, R"({"max_output_tokens":999,"llm_model":123})");
    EXPECT_EQ(merged.max_output_tokens, kContextualMaxOutputTokensMax);
    EXPECT_EQ(merged.llm_model, kContextualDefaultLlmModel);  // unchanged
}

TEST(F35ContextualConfigTest, MergeNsOverrideMalformedJsonIsNoOp) {
    ContextualRetrievalConfig base;
    base.llm_model = "gpt-4o";
    ContextualRetrievalConfig merged = MergeNsOverride(base, "{not valid json");
    EXPECT_EQ(merged.llm_model, "gpt-4o");
}

TEST(F35ContextualConfigTest, MergeNsOverrideEmptyModelIgnored) {
    ContextualRetrievalConfig base;
    base.llm_model = "gpt-4o";
    ContextualRetrievalConfig merged = MergeNsOverride(base, R"({"llm_model":""})");
    EXPECT_EQ(merged.llm_model, "gpt-4o");  // empty override ignored
}

// ---------- ContextualOnnxEmbedder adapter (null-guard; real ONNX path = D3.5) ----------

TEST(F35ContextualOnnxEmbedderTest, NullOnnxEmbedderIsUnavailable) {
    // The adapter over a null OnnxEmbedder reports Unavailable rather than crashing.
    // The real-model success path needs an initialized OnnxEmbedder (ONNX runtime) →
    // exercised at D3.5, not in standalone.
    ContextualOnnxEmbedder adapter(/*embedder=*/nullptr);
    auto r = adapter.Embed("some text");
    EXPECT_FALSE(r.ok());
    EXPECT_EQ(r.status().code(), StatusCode::kUnavailable);
}

TEST(F35ContextualOnnxEmbedderTest, NonNullEmbedderPropagatesStubEmbedding) {
    // A real (stub-mode) OnnxEmbedder behind the adapter: the embedder_ != nullptr
    // branch + the underlying Embed() success path produce a vector.
    auto onnx = std::make_shared<cortrix::OnnxEmbedder>("", 16);
    ASSERT_TRUE(onnx->Init().ok());
    ContextualOnnxEmbedder adapter(onnx);
    auto r = adapter.Embed("hello world");
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_EQ(static_cast<int>(r.value().size()), 16);
}

TEST(F35ContextualOnnxEmbedderTest, NonNullEmbedderPropagatesFailure) {
    // embedder_ != nullptr but the underlying Embed fails (not initialized) → the
    // `if (!s.ok()) return s` branch surfaces the error verbatim.
    auto onnx = std::make_shared<cortrix::OnnxEmbedder>("", 16);  // no Init()
    ContextualOnnxEmbedder adapter(onnx);
    auto r = adapter.Embed("hello");
    EXPECT_FALSE(r.ok());
}

// ---------- MergeNsOverride: per-field type-guard false branches ----------

// Each NS field present but the WRONG json type is independently ignored (the
// type-check false arm), leaving the base value unchanged.
TEST(F35ContextualConfigTest, MergeNsOverrideWrongTypesAllIgnored) {
    ContextualRetrievalConfig base;
    base.enabled = true;
    base.prompt_template = "BASE T";
    base.max_output_tokens = 90;
    base.llm_model = "base-model";

    // enabled as string, prompt_template as number, max_output_tokens as string,
    // llm_model as bool — all wrong types → every field keeps the base value.
    ContextualRetrievalConfig merged = MergeNsOverride(
        base,
        R"({"enabled":"yes","prompt_template":5,"max_output_tokens":"big","llm_model":true})");
    EXPECT_TRUE(merged.enabled);
    EXPECT_EQ(merged.prompt_template, "BASE T");
    EXPECT_EQ(merged.max_output_tokens, 90);
    EXPECT_EQ(merged.llm_model, "base-model");
}

// An NS llm_model present, correct type, but EMPTY string → the `!empty()` guard
// rejects it (the empty-string false arm), base value kept.
TEST(F35ContextualConfigTest, MergeNsOverrideEmptyLlmModelIgnored) {
    ContextualRetrievalConfig base;
    base.llm_model = "keep-me";
    ContextualRetrievalConfig merged = MergeNsOverride(base, R"({"llm_model":""})");
    EXPECT_EQ(merged.llm_model, "keep-me");
}

// "contextual_retrieval" key present but NOT an object → the nested-node guard is
// false, so resolution falls back to treating the root as flat.
TEST(F35ContextualConfigTest, MergeNsOverrideNestedNotObjectFallsBackToFlat) {
    ContextualRetrievalConfig base;
    base.max_output_tokens = 80;
    // contextual_retrieval is a string (not object) → node stays = root; a flat
    // max_output_tokens at the root level is then applied.
    ContextualRetrievalConfig merged = MergeNsOverride(
        base, R"({"contextual_retrieval":"oops","max_output_tokens":150})");
    EXPECT_EQ(merged.max_output_tokens, 150);
}

// A non-object (but valid) JSON root → fail-soft no-op (the !is_object() arm).
TEST(F35ContextualConfigTest, MergeNsOverrideNonObjectRootIsNoOp) {
    ContextualRetrievalConfig base;
    base.max_output_tokens = 77;
    ContextualRetrievalConfig merged = MergeNsOverride(base, "[1,2,3]");  // array root
    EXPECT_EQ(merged.max_output_tokens, 77);
}

// BuildPrompt with a custom NS template substitutes every {{...}} marker (the
// non-empty prompt_template branch + the ReplaceAll non-empty-token path).
TEST(F35ContextualEnricherTest, BuildPromptCustomTemplateSubstitutesMarkers) {
    ContextualRetrievalConfig cfg;
    cfg.prompt_template =
        "T={{doc_title}} S={{section_heading}} P={{prev_chunk_text}} "
        "C={{chunk_text}} N={{next_chunk_text}}";
    auto llm = std::make_shared<llm::MockLlmClient>();
    EXPECT_CALL(*llm, Chat(::testing::_, ::testing::_))
        .WillOnce(::testing::Return(OkChat("ctx")));
    ContextualRetrievalEnricher e(cfg, llm, MakeEmbedder());
    // Drive BuildPrompt through Enrich (BuildPrompt is private); the LLM mock just
    // returns a short context, so Enrich reaches the embedding step.
    DocumentMetadata meta = DocMeta("MyDoc");
    EnrichResult res = e.Enrich("BODY", meta, MakeCtx("BODY", "PREV", "NEXT"));
    EXPECT_EQ(res.contextualized_status, 1);  // generated (full path succeeded)
}

}  // namespace
}  // namespace cortrix::spc
