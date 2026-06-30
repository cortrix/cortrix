#include "mock_response_builder.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <memory>

#include <nlohmann/json.hpp>

#include "cortrix/llm/llm_error_tokens.h"
#include "cortrix/spc_enricher.h"
#include "cortrix/spc_enricher/enricher_error.h"
#include "cortrix/spc/parser.h"
#include "mock_llm_client.h"

namespace cortrix::llm {
namespace {

using ::testing::_;
using ::testing::Return;

// --- The 8 MockResponseBuilder factories (Issue 5.2) ---------------------------

TEST(MockResponseBuilderTest, NormalProducesValidIndexedJson) {
    auto r = MockResponseBuilder::Normal(3, "gpt-4o-mini");
    EXPECT_TRUE(r.ok());
    EXPECT_EQ(r.model, "gpt-4o-mini");
    EXPECT_GT(r.prompt_tokens, 0);
    auto j = nlohmann::json::parse(r.content);
    ASSERT_TRUE(j.is_object());
    EXPECT_TRUE(j.contains("0"));
    EXPECT_TRUE(j.contains("2"));
    EXPECT_EQ(j["0"]["entities"][0]["type"], "ORG");
}

TEST(MockResponseBuilderTest, EmptyMalformedAreOkButUnparseable) {
    EXPECT_TRUE(MockResponseBuilder::Empty().ok());
    EXPECT_TRUE(MockResponseBuilder::Empty().content.empty());

    auto m = MockResponseBuilder::MalformedJson();
    EXPECT_TRUE(m.ok());
    auto j = nlohmann::json::parse(m.content, nullptr, false);
    EXPECT_TRUE(j.is_discarded());  // not valid JSON
}

TEST(MockResponseBuilderTest, PartialBatchHasOnlyReturnedIndices) {
    auto r = MockResponseBuilder::PartialBatch(1, 3);
    auto j = nlohmann::json::parse(r.content);
    EXPECT_TRUE(j.contains("0"));
    EXPECT_FALSE(j.contains("1"));
    EXPECT_FALSE(j.contains("2"));
}

TEST(MockResponseBuilderTest, ErrorBuildersCarryNeutralTokens) {
    EXPECT_FALSE(MockResponseBuilder::Timeout().ok());
    EXPECT_EQ(MockResponseBuilder::Timeout().status.message().rfind(llm_tokens::kTransport, 0), 0u);

    auto e503 = MockResponseBuilder::HttpError(503);
    EXPECT_EQ(e503.status.code(), StatusCode::kUnavailable);
    EXPECT_EQ(e503.status.message().rfind(llm_tokens::kHttp, 0), 0u);

    auto e400 = MockResponseBuilder::HttpError(400);
    EXPECT_EQ(e400.status.code(), StatusCode::kInternal);

    auto rl = MockResponseBuilder::RateLimit(30);
    EXPECT_EQ(rl.status.message().rfind(llm_tokens::kRateLimit, 0), 0u);
    EXPECT_EQ(rl.finish_reason, "30");
}

TEST(MockResponseBuilderTest, BudgetExceededIsHighUsageOk) {
    auto b = MockResponseBuilder::BudgetExceeded();
    EXPECT_TRUE(b.ok());
    EXPECT_GT(b.prompt_tokens, 1'000'000);
}

// --- The builders driving the real LlmEnricher path via MockLlmClient ---------
// (Issue 5: shared Mock module exercises the enricher end-to-end, no network.)

spc::EnricherConfig LlmCfg() {
    spc::EnricherConfig cfg;
    cfg.type = spc::EnricherType::kLlm;
    cfg.enabled = true;
    cfg.model = "gpt-4o-mini";
    cfg.batch_size = 8;
    cfg.circuit_breaker_enabled = false;  // isolate parse behavior
    return cfg;
}

std::vector<spc::ChunkContext> Batch(const spc::DocumentMetadata& meta, int n) {
    std::vector<spc::ChunkContext> out;
    for (int i = 0; i < n; ++i) {
        spc::ChunkContext c;
        c.chunk_text = "chunk " + std::to_string(i);
        c.chunk_index = i;
        c.doc_metadata = &meta;
        out.push_back(c);
    }
    return out;
}

TEST(MockResponseBuilderDrivesEnricher, NormalAllSucceed) {
    auto mock = std::make_shared<MockLlmClient>();
    EXPECT_CALL(*mock, Chat(_, _)).WillOnce(Return(MockResponseBuilder::Normal(3)));
    spc::DocumentMetadata meta;
    spc::LlmEnricher enr(LlmCfg(), mock);
    auto r = enr.EnrichBatch(Batch(meta, 3));
    ASSERT_EQ(r.size(), 3u);
    for (const auto& res : r) {
        EXPECT_TRUE(res.ok());
        EXPECT_FALSE(res.summary.empty());
    }
}

TEST(MockResponseBuilderDrivesEnricher, PartialBatchTriggersL2) {
    auto mock = std::make_shared<MockLlmClient>();
    // F03 now retries each parse-failed chunk individually (RunOneBatch, n==1) when
    // the batch response is partial — so Chat() is called once for the batch plus
    // once per failed chunk. The single-item retries return malformed output, so the
    // missing chunks stay parse failures and the final result set is unchanged.
    EXPECT_CALL(*mock, Chat(_, _))
        .WillOnce(Return(MockResponseBuilder::PartialBatch(1, 3)))
        .WillRepeatedly(Return(MockResponseBuilder::MalformedJson()));
    spc::DocumentMetadata meta;
    spc::LlmEnricher enr(LlmCfg(), mock);
    auto r = enr.EnrichBatch(Batch(meta, 3));
    ASSERT_EQ(r.size(), 3u);
    EXPECT_TRUE(r[0].ok());                                                  // present
    EXPECT_EQ(r[1].status, static_cast<int>(spc::EnricherErrorCode::kParse));  // L2 missing
    EXPECT_EQ(r[2].status, static_cast<int>(spc::EnricherErrorCode::kParse));
}

TEST(MockResponseBuilderDrivesEnricher, MalformedTriggersL3) {
    auto mock = std::make_shared<MockLlmClient>();
    // Whole-batch malformed → each chunk is retried individually (RunOneBatch,
    // n==1); the retries are malformed too, so every chunk stays a parse failure.
    EXPECT_CALL(*mock, Chat(_, _))
        .WillOnce(Return(MockResponseBuilder::MalformedJson()))
        .WillRepeatedly(Return(MockResponseBuilder::MalformedJson()));
    spc::DocumentMetadata meta;
    spc::LlmEnricher enr(LlmCfg(), mock);
    auto r = enr.EnrichBatch(Batch(meta, 2));
    ASSERT_EQ(r.size(), 2u);
    for (const auto& res : r) {
        EXPECT_EQ(res.status, static_cast<int>(spc::EnricherErrorCode::kParse));
        auto sd = nlohmann::json::parse(res.error_meta.structured_data);
        EXPECT_EQ(sd["layer"], "L3");
    }
}

}  // namespace
}  // namespace cortrix::llm
