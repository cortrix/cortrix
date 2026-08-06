#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "cortrix/llm/i_llm_client.h"
#include "cortrix/llm/llm_error_tokens.h"
#include "cortrix/spc_enricher.h"
#include "cortrix/spc_enricher/enricher_error.h"
#include "cortrix/spc/parser.h"
#include "fake_http_transport.h"
#include "mock_llm_client.h"

namespace cortrix::spc {
namespace {

using ::testing::_;
using ::testing::DoAll;
using ::testing::InSequence;
using ::testing::Return;
using ::testing::SaveArg;

llm::ChatCompletionResponse OkChat(std::string content, int pt = 10, int ct = 5) {
    llm::ChatCompletionResponse r;
    r.content = std::move(content);
    r.finish_reason = "stop";
    r.prompt_tokens = pt;
    r.completion_tokens = ct;
    r.status = Status::Ok();
    return r;
}

std::vector<ChunkContext> Batch(const DocumentMetadata& meta, int n) {
    std::vector<ChunkContext> out;
    for (int i = 0; i < n; ++i) {
        ChunkContext c;
        c.chunk_text = "chunk text " + std::to_string(i);
        c.chunk_index = i;
        c.doc_metadata = &meta;
        out.push_back(c);
    }
    return out;
}

EnricherConfig LlmCfg() {
    EnricherConfig cfg;
    cfg.type = EnricherType::kLlm;
    cfg.enabled = true;
    cfg.model = "gpt-4o-mini";
    cfg.batch_size = 8;
    cfg.prompt_template_id = "default-zh";
    cfg.http_retry_backoff_ms = 1;  // seconds-level default would stall tests
    return cfg;
}

TEST(LlmEnricherTest, AvailableWhenEnabledAndClientPresent) {
    auto mock = std::make_shared<llm::MockLlmClient>();
    LlmEnricher enr(LlmCfg(), mock);
    EXPECT_TRUE(enr.IsAvailable());
    EXPECT_EQ(enr.Name(), "LlmEnricher");
}

TEST(LlmEnricherTest, NotAvailableWhenDisabled) {
    auto cfg = LlmCfg();
    cfg.enabled = false;
    LlmEnricher enr(cfg, std::make_shared<llm::MockLlmClient>());
    EXPECT_FALSE(enr.IsAvailable());
}

TEST(LlmEnricherTest, NotAvailableWhenClientNull) {
    LlmEnricher enr(LlmCfg(), nullptr);
    EXPECT_FALSE(enr.IsAvailable());
}

TEST(LlmEnricherTest, EnrichBatchSuccessFillsTwelveFields) {
    auto mock = std::make_shared<llm::MockLlmClient>();
    EXPECT_CALL(*mock, Chat(_, _))
        .WillOnce(Return(OkChat(
            R"({"0":{"entities":[{"text":"Cortrix","type":"PRODUCT","start_offset":0,"end_offset":7}],
                    "summary":"about Cortrix","score":0.9},
                "1":{"entities":[],"summary":"second","score":0.4}})",
            /*pt=*/30, /*ct=*/12)));

    DocumentMetadata meta;
    meta.filename = "doc.pdf";
    LlmEnricher enr(LlmCfg(), mock);
    auto results = enr.EnrichBatch(Batch(meta, 2));

    ASSERT_EQ(results.size(), 2u);
    // chunk 0
    EXPECT_TRUE(results[0].ok());
    ASSERT_EQ(results[0].entities.size(), 1u);
    EXPECT_EQ(results[0].entities[0].text, "Cortrix");
    EXPECT_EQ(results[0].summary, "about Cortrix");
    EXPECT_FLOAT_EQ(results[0].enriched_score, 0.9f);
    // 12-field metadata stamped
    EXPECT_EQ(results[0].model_used, "gpt-4o-mini");
    EXPECT_EQ(results[0].prompt_version, "default-zh");
    EXPECT_EQ(results[0].enricher_name, "LlmEnricher");
    EXPECT_EQ(results[0].token_count, 42);   // 30 + 12 for the batch call
    EXPECT_GT(results[0].enriched_at, 0);
    EXPECT_GE(results[0].duration_ms, 0);
    // chunk 1
    EXPECT_EQ(results[1].summary, "second");
    EXPECT_FLOAT_EQ(results[1].enriched_score, 0.4f);
}

TEST(LlmEnricherTest, DeepSeekStructuredCallDisablesThinkingAndUsesLargeTokenBudget) {
    auto mock = std::make_shared<llm::MockLlmClient>();
    llm::LlmCallConfig captured;
    EXPECT_CALL(*mock, Chat(_, _))
        .WillOnce(DoAll(SaveArg<1>(&captured),
                        Return(OkChat(R"({"0":{"summary":"ok","score":0.9}})"))));

    DocumentMetadata meta;
    auto cfg = LlmCfg();
    cfg.model = "deepseek-v4-flash";
    LlmEnricher enr(cfg, mock);
    auto results = enr.EnrichBatch(Batch(meta, 1));

    ASSERT_EQ(results.size(), 1u);
    EXPECT_TRUE(results[0].ok());
    EXPECT_EQ(captured.max_tokens, 4096);
    EXPECT_EQ(captured.response_format, "json_object");
    EXPECT_EQ(captured.thinking_type, "disabled");
    EXPECT_FALSE(captured.allow_reasoning_content_fallback);
}

// operational relief: the batch-call response budget is a config knob now
// (default stays 4096); deployments size it with batch_size instead of hitting
// the hardcoded ceiling (~128 tokens/chunk at batch 32 truncated the JSON).
TEST(LlmEnricherTest, ConfiguredMaxTokensFlowsToCall) {
    auto mock = std::make_shared<llm::MockLlmClient>();
    llm::LlmCallConfig captured;
    EXPECT_CALL(*mock, Chat(_, _))
        .WillOnce(DoAll(SaveArg<1>(&captured),
                        Return(OkChat(R"({"0":{"summary":"ok","score":0.9}})"))));

    DocumentMetadata meta;
    auto cfg = LlmCfg();
    cfg.max_tokens = 8192;
    LlmEnricher enr(cfg, mock);
    auto results = enr.EnrichBatch(Batch(meta, 1));

    ASSERT_EQ(results.size(), 1u);
    EXPECT_TRUE(results[0].ok());
    EXPECT_EQ(captured.max_tokens, 8192);
}

// QA 2026-07-12 F-7: an absurd configured value must not ride into the provider
// request (it would 400 every batch) — the slot clamps to kEnricherMaxTokensCap.
TEST(LlmEnricherTest, OversizeMaxTokensClampedAtSlot) {
    auto mock = std::make_shared<llm::MockLlmClient>();
    llm::LlmCallConfig captured;
    EXPECT_CALL(*mock, Chat(_, _))
        .WillOnce(DoAll(SaveArg<1>(&captured),
                        Return(OkChat(R"({"0":{"summary":"ok","score":0.9}})"))));

    DocumentMetadata meta;
    auto cfg = LlmCfg();
    cfg.max_tokens = 1000000000;  // nonsensical yaml value
    LlmEnricher enr(cfg, mock);
    auto results = enr.EnrichBatch(Batch(meta, 1));

    ASSERT_EQ(results.size(), 1u);
    EXPECT_TRUE(results[0].ok());
    EXPECT_EQ(captured.max_tokens, kEnricherMaxTokensCap);
}

TEST(LlmEnricherTest, BatchSplitByBatchSize) {
    auto mock = std::make_shared<llm::MockLlmClient>();
    auto cfg = LlmCfg();
    cfg.batch_size = 2;  // 5 chunks → 3 calls (2 + 2 + 1)
    // Each call returns a 2-keyed object; the parser pads/truncates per call size.
    EXPECT_CALL(*mock, Chat(_, _))
        .Times(3)
        .WillRepeatedly(Return(OkChat(
            R"({"0":{"summary":"s0","score":0.5},"1":{"summary":"s1","score":0.5}})")));

    DocumentMetadata meta;
    LlmEnricher enr(cfg, mock);
    auto results = enr.EnrichBatch(Batch(meta, 5));
    EXPECT_EQ(results.size(), 5u);  // one result per input chunk across the 3 calls
}

TEST(LlmEnricherTest, LlmTransportFailureProducesLlmApiErrors) {
    auto mock = std::make_shared<llm::MockLlmClient>();
    llm::ChatCompletionResponse fail;
    fail.status = Status::Unavailable(std::string(llm::llm_tokens::kTransport) +
                                      ": connect refused");
    // Transport failures are busy-retried (up to 3 attempts) → the mock
    // must answer every attempt with the failure (WillRepeatedly).
    EXPECT_CALL(*mock, Chat(_, _)).WillRepeatedly(Return(fail));

    DocumentMetadata meta;
    LlmEnricher enr(LlmCfg(), mock);
    auto results = enr.EnrichBatch(Batch(meta, 2));
    ASSERT_EQ(results.size(), 2u);
    for (const auto& r : results) {
        EXPECT_EQ(r.status, static_cast<int>(EnricherErrorCode::kLlmApi));
        EXPECT_FLOAT_EQ(r.enriched_score, 0.0f);
        EXPECT_TRUE(r.error_meta.retryable);  // LLM_API is transient/retryable
        EXPECT_EQ(r.error_meta.category, agent_friendly::ErrorCategory::kTransient);
    }
}

TEST(LlmEnricherTest, RateLimitFailureProducesRateLimitErrors) {
    auto mock = std::make_shared<llm::MockLlmClient>();
    llm::ChatCompletionResponse fail;
    fail.status = Status::Unavailable(std::string(llm::llm_tokens::kRateLimit) + ": 429");
    EXPECT_CALL(*mock, Chat(_, _)).WillOnce(Return(fail));

    DocumentMetadata meta;
    LlmEnricher enr(LlmCfg(), mock);
    auto results = enr.EnrichBatch(Batch(meta, 1));
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].status, static_cast<int>(EnricherErrorCode::kRateLimit));
    EXPECT_TRUE(results[0].error_meta.retryable);
}

TEST(LlmEnricherTest, EnrichSingleGoesThroughBatchOne) {
    auto mock = std::make_shared<llm::MockLlmClient>();
    EXPECT_CALL(*mock, Chat(_, _))
        .WillOnce(Return(OkChat(R"({"0":{"summary":"single","score":0.7}})")));

    DocumentMetadata meta;
    ChunkContext ctx;
    ctx.chunk_text = "hello";
    ctx.doc_metadata = &meta;
    LlmEnricher enr(LlmCfg(), mock);
    auto r = enr.Enrich("hello", meta, ctx);
    EXPECT_TRUE(r.ok());
    EXPECT_EQ(r.summary, "single");
    EXPECT_FLOAT_EQ(r.enriched_score, 0.7f);
}

TEST(LlmEnricherTest, EmptyBatchNoCall) {
    auto mock = std::make_shared<llm::MockLlmClient>();
    EXPECT_CALL(*mock, Chat(_, _)).Times(0);
    LlmEnricher enr(LlmCfg(), mock);
    EXPECT_TRUE(enr.EnrichBatch({}).empty());
}

// Enrich() single-chunk fallback wiring: an empty ctx.chunk_text takes the
// chunk_text argument (the `one.chunk_text.empty()` true branch) and a null
// ctx.doc_metadata takes the doc_meta argument (the null-meta true branch).
TEST(LlmEnricherTest, EnrichSingleFillsEmptyCtxFromArgs) {
    auto mock = std::make_shared<llm::MockLlmClient>();
    EXPECT_CALL(*mock, Chat(_, _))
        .WillOnce(Return(OkChat(R"({"0":{"summary":"filled","score":0.6}})")));

    DocumentMetadata meta;
    meta.filename = "from_arg.pdf";
    ChunkContext ctx;            // chunk_text empty, doc_metadata == nullptr
    LlmEnricher enr(LlmCfg(), mock);
    auto r = enr.Enrich("arg chunk body", meta, ctx);
    EXPECT_TRUE(r.ok());
    EXPECT_EQ(r.summary, "filled");
}

// A whole-batch L3 parse failure (LLM returned non-JSON): every chunk gets the
// kParse status, the parse-failure metric branch fires, and the per-result
// default-field fills run (model_used / enricher_name / prompt_version were left
// empty by the failed parse → stamped here).
TEST(LlmEnricherTest, ParseFailureStampsDefaultsAndRecordsParseMetric) {
    auto mock = std::make_shared<llm::MockLlmClient>();
    EXPECT_CALL(*mock, Chat(_, _))
        .WillOnce(Return(OkChat("this is not valid JSON at all", /*pt=*/5, /*ct=*/5)));

    DocumentMetadata meta;
    LlmEnricher enr(LlmCfg(), mock);
    auto results = enr.EnrichBatch(Batch(meta, 1));
    ASSERT_EQ(results.size(), 1u);
    for (const auto& r : results) {
        EXPECT_EQ(r.status, static_cast<int>(EnricherErrorCode::kParse));
        // Defaults were stamped even though the parse failed.
        EXPECT_EQ(r.model_used, "gpt-4o-mini");
        EXPECT_EQ(r.enricher_name, "LlmEnricher");
        EXPECT_EQ(r.prompt_version, "default-zh");
        EXPECT_EQ(r.token_count, 10);  // 5 + 5
    }
}

TEST(LlmEnricherTest, RetriesWholeBatchParseFailureBySingleChunk) {
    auto mock = std::make_shared<llm::MockLlmClient>();
    {
        InSequence seq;
        EXPECT_CALL(*mock, Chat(_, _))
            .WillOnce(Return(OkChat(R"({"0":{"summary":"truncated")")));
        EXPECT_CALL(*mock, Chat(_, _))
            .WillOnce(Return(OkChat(R"({"0":{"entities":[],"summary":"retry zero","score":0.8}})")));
        EXPECT_CALL(*mock, Chat(_, _))
            .WillOnce(Return(OkChat(R"({"0":{"entities":[],"summary":"retry one","score":0.7}})")));
    }

    DocumentMetadata meta;
    auto cfg = LlmCfg();
    cfg.batch_size = 2;
    LlmEnricher enr(cfg, mock);
    auto results = enr.EnrichBatch(Batch(meta, 2));

    ASSERT_EQ(results.size(), 2u);
    EXPECT_TRUE(results[0].ok());
    EXPECT_TRUE(results[1].ok());
    EXPECT_EQ(results[0].summary, "retry zero");
    EXPECT_EQ(results[1].summary, "retry one");
    EXPECT_FLOAT_EQ(results[0].enriched_score, 0.8f);
    EXPECT_FLOAT_EQ(results[1].enriched_score, 0.7f);
}

TEST(LlmEnricherTest, RetriesMissingChunkIndexBySingleChunk) {
    auto mock = std::make_shared<llm::MockLlmClient>();
    {
        InSequence seq;
        EXPECT_CALL(*mock, Chat(_, _))
            .WillOnce(Return(OkChat(
                R"({"0":{"entities":[],"summary":"first pass zero","score":0.6}})")));
        EXPECT_CALL(*mock, Chat(_, _))
            .WillOnce(Return(OkChat(
                R"({"0":{"entities":[],"summary":"retried missing one","score":0.5}})")));
    }

    DocumentMetadata meta;
    auto cfg = LlmCfg();
    cfg.batch_size = 2;
    LlmEnricher enr(cfg, mock);
    auto results = enr.EnrichBatch(Batch(meta, 2));

    ASSERT_EQ(results.size(), 2u);
    EXPECT_TRUE(results[0].ok());
    EXPECT_TRUE(results[1].ok());
    EXPECT_EQ(results[0].summary, "first pass zero");
    EXPECT_EQ(results[1].summary, "retried missing one");
    EXPECT_FLOAT_EQ(results[0].enriched_score, 0.6f);
    EXPECT_FLOAT_EQ(results[1].enriched_score, 0.5f);
}

// Factory: kLlm + api_key + probe-OK builds an LlmEnricher (uses the S4.2 probe
// seam so no live network is hit). The probe/fallback paths are covered in
// test_enricher_startup.cpp.
TEST(CreateEnricherTest, LlmWithApiKeyAndProbeOkMakesLlmEnricher) {
    auto cfg = LlmCfg();
    cfg.api_key = "sk-test";
    llm::FakeHttpTransport probe;
    probe.canned = llm::FakeHttpTransport::Json2xx(R"({"data":[]})");  // /models OK
    auto e = CreateEnricher(cfg, probe);
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->Name(), "LlmEnricher");
}

TEST(CreateEnricherTest, LlmWithoutApiKeyDegradesToNull) {
    auto cfg = LlmCfg();
    cfg.api_key = "";  // no key → no point calling → Null fallback (scenario 2)
    llm::FakeHttpTransport probe;  // not consulted (short-circuits before probe)
    auto e = CreateEnricher(cfg, probe);
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->Name(), "NullEnricher");
}

}  // namespace
}  // namespace cortrix::spc
