#include "cortrix/spc_enricher.h"

#include <gtest/gtest.h>

#include <memory>
#include <vector>

#include "cortrix/agent_friendly/error.h"
#include "cortrix/spc/parser.h"

namespace cortrix::spc {
namespace {

// Build a small ChunkContext batch over a shared DocumentMetadata (non-owning
// pointer, per the §2.4 reconcile — meta must outlive the contexts).
std::vector<ChunkContext> MakeBatch(const DocumentMetadata& meta, int n) {
    std::vector<ChunkContext> out;
    for (int i = 0; i < n; ++i) {
        ChunkContext c;
        c.chunk_text = "chunk " + std::to_string(i);
        c.chunk_index = i;
        c.doc_metadata = &meta;
        c.prev_chunk_text = (i == 0) ? "" : "chunk " + std::to_string(i - 1);
        c.next_chunk_text = (i == n - 1) ? "" : "chunk " + std::to_string(i + 1);
        out.push_back(c);  // requires ChunkContext be copyable (reconcile DoD)
    }
    return out;
}

// --- EnrichResult / Entity defaults (12 fields) ------------------------------

TEST(EnrichResultTest, DefaultIsSuccessEmpty) {
    EnrichResult r;
    EXPECT_EQ(r.status, 0);
    EXPECT_TRUE(r.ok());
    EXPECT_TRUE(r.entities.empty());
    EXPECT_TRUE(r.summary.empty());
    EXPECT_FLOAT_EQ(r.enriched_score, 0.0f);
    EXPECT_TRUE(r.error_msg.empty());
    // Issue 1.4 added 6 field defaults
    EXPECT_TRUE(r.prompt_version.empty());
    EXPECT_TRUE(r.model_used.empty());
    EXPECT_EQ(r.duration_ms, 0);
    EXPECT_EQ(r.token_count, 0);
    EXPECT_TRUE(r.enricher_name.empty());
    EXPECT_EQ(r.enriched_at, 0);
    // error_meta default (status==0 → unused)
    EXPECT_FALSE(r.error_meta.retryable);
    EXPECT_EQ(r.error_meta.retry_after_ms, -1);
}

TEST(EntityTest, HoldsTextTypeOffsets) {
    Entity e{"Cortrix", "PRODUCT", 0, 7};
    EXPECT_EQ(e.text, "Cortrix");
    EXPECT_EQ(e.type, "PRODUCT");
    EXPECT_EQ(e.start_offset, 0);
    EXPECT_EQ(e.end_offset, 7);
}

// --- EnricherErrorMeta::FromCode ---------------------------------------------

TEST(EnricherErrorMetaTest, FromCodeRegistryDriven) {
    auto meta = EnricherErrorMeta::FromCode(
        EnricherErrorCode::kLlmTimeout, R"({"model":"gpt-4o-mini"})");
    EXPECT_TRUE(meta.retryable);
    EXPECT_EQ(meta.category, agent_friendly::ErrorCategory::kTimeout);
    EXPECT_EQ(meta.retry_after_ms, 1000);
    EXPECT_EQ(meta.structured_data, R"({"model":"gpt-4o-mini"})");

    // Non-retryable → -1 sentinel (design §2.5b "0 / -1 = N/A").
    auto meta2 = EnricherErrorMeta::FromCode(EnricherErrorCode::kBudget);
    EXPECT_FALSE(meta2.retryable);
    EXPECT_EQ(meta2.category, agent_friendly::ErrorCategory::kQuota);
    EXPECT_EQ(meta2.retry_after_ms, -1);
}

// --- EnricherType parse / string roundtrip -----------------------------------

TEST(EnricherTypeTest, ParseAndString) {
    EXPECT_EQ(ParseEnricherType("null"), EnricherType::kNull);
    EXPECT_EQ(ParseEnricherType("llm"), EnricherType::kLlm);
    EXPECT_EQ(ParseEnricherType("local_ner"), EnricherType::kLocalNer);
    EXPECT_EQ(ParseEnricherType("garbage"), EnricherType::kNull);  // fail-soft

    EXPECT_STREQ(EnricherTypeString(EnricherType::kNull), "null");
    EXPECT_STREQ(EnricherTypeString(EnricherType::kLlm), "llm");
    EXPECT_STREQ(EnricherTypeString(EnricherType::kLocalNer), "local_ner");
}

TEST(EnricherConfigTest, DefaultsMatchGucTable) {
    EnricherConfig c;
    // Category A
    EXPECT_EQ(c.type, EnricherType::kNull);
    EXPECT_EQ(c.endpoint, "https://api.openai.com/v1");
    EXPECT_EQ(c.workers, 4);
    EXPECT_EQ(c.queue_size, 100);
    EXPECT_EQ(c.task_timeout_ms, 30000);
    EXPECT_EQ(c.batch_size, 8);
    EXPECT_TRUE(c.circuit_breaker_enabled);
    EXPECT_EQ(c.circuit_breaker_threshold, 10);
    EXPECT_EQ(c.circuit_breaker_cooldown_sec, 60);
    EXPECT_EQ(c.budget_cap_usd, 0);
    EXPECT_EQ(c.startup_probe_timeout_ms, 5000);
    // Category B
    EXPECT_TRUE(c.enabled);
    EXPECT_EQ(c.model, "gpt-4o-mini");
    EXPECT_FLOAT_EQ(c.score_threshold, 0.0f);
    EXPECT_EQ(c.prompt_template_id, "default-zh");
}

// --- NullEnricher behavior ---------------------------------------------------

TEST(NullEnricherTest, IsNotAvailableAndNamed) {
    NullEnricher null;
    EXPECT_FALSE(null.IsAvailable());
    EXPECT_EQ(null.Name(), "NullEnricher");
}

TEST(NullEnricherTest, EnrichReturnsEmptySuccess) {
    NullEnricher null;
    DocumentMetadata meta;
    meta.filename = "doc.pdf";
    ChunkContext ctx;
    ctx.chunk_text = "hello world";
    ctx.doc_metadata = &meta;

    EnrichResult r = null.Enrich("hello world", meta, ctx);
    EXPECT_TRUE(r.ok());
    EXPECT_TRUE(r.entities.empty());
    EXPECT_TRUE(r.summary.empty());
    EXPECT_FLOAT_EQ(r.enriched_score, 0.0f);
}

TEST(NullEnricherTest, EnrichBatchReturnsOnePerContext) {
    NullEnricher null;
    DocumentMetadata meta;
    auto batch = MakeBatch(meta, 5);
    auto results = null.EnrichBatch(batch);
    ASSERT_EQ(results.size(), 5u);
    for (const auto& r : results) {
        EXPECT_TRUE(r.ok());
        EXPECT_TRUE(r.entities.empty());
    }

    // Empty batch → empty results.
    EXPECT_TRUE(null.EnrichBatch({}).empty());
}

TEST(NullEnricherTest, UsableThroughBaseInterface) {
    std::unique_ptr<ISpcEnricher> e = std::make_unique<NullEnricher>();
    EXPECT_FALSE(e->IsAvailable());
    EXPECT_EQ(e->Name(), "NullEnricher");
}

// --- Factory -----------------------------------------------------------------

TEST(CreateEnricherTest, NullTypeMakesNullEnricher) {
    EnricherConfig cfg;
    cfg.type = EnricherType::kNull;
    auto e = CreateEnricher(cfg);
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->Name(), "NullEnricher");
    EXPECT_FALSE(e->IsAvailable());
}

TEST(CreateEnricherTest, LocalNerDegradesToNullInV1) {
    EnricherConfig cfg;
    cfg.type = EnricherType::kLocalNer;  // Phase 2 — not implemented in V1
    auto e = CreateEnricher(cfg);
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->Name(), "NullEnricher");
}

// kLlm with no api_key degrades to NullEnricher (§4.4 fail-soft — no point
// calling without credentials). The kLlm + api_key → LlmEnricher path is covered
// in test_llm_enricher.cpp (CreateEnricherTest.LlmWithApiKeyMakesLlmEnricher).
TEST(CreateEnricherTest, LlmTypeWithoutApiKeyDegradesToNull) {
    EnricherConfig cfg;
    cfg.type = EnricherType::kLlm;  // api_key empty by default
    auto e = CreateEnricher(cfg);
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->Name(), "NullEnricher");
}

}  // namespace
}  // namespace cortrix::spc
