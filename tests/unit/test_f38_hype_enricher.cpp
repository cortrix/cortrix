#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include <memory>
#include <string>
#include <vector>

#include "cortrix/spc/hype_enricher.h"
#include "cortrix/store/store_errors.h"
#include "mock_llm_client.h"
#include "mock_parent_chunk_store.h"

// F38 S1 — HyPEEnricher skeleton: ISpcEnricher impl (Enrich + EnrichBatch +
// IsAvailable + Name), GenerateHypeQuestions (LLM mock), ResolveParentText
// (ParentChunkStore mock, optional-context two paths). Enrich() returns a STANDARD
// EnrichResult (reconcile 1); parent_text is optional (reconcile 2).
namespace cortrix::spc {
namespace {

using ::testing::_;
using ::testing::Return;

llm::ChatCompletionResponse OkChat(std::string content) {
    llm::ChatCompletionResponse r;
    r.content = std::move(content);
    r.model = "gpt-4o-mini";
    r.prompt_tokens = 20;
    r.completion_tokens = 15;
    return r;  // status defaults ok()
}

cortrix::chunker::ParentChunk MakeParent(std::string text) {
    cortrix::chunker::ParentChunk p;
    p.parent_id = "p1";
    p.parent_text = std::move(text);
    return p;
}

DocumentMetadata DocMeta(std::string title = "Annual Report") {
    DocumentMetadata m;
    m.doc_title = std::move(title);
    return m;
}

// ---------- ISpcEnricher contract ----------

TEST(F38HyPEEnricherTest, UsableViaBasePointer) {
    auto llm = std::make_shared<llm::MockLlmClient>();
    HyPEEnricher e(HyPEConfig{}, llm, nullptr);
    ISpcEnricher* base = &e;  // polymorphic use through the frozen interface
    EXPECT_EQ(base->Name(), "hype");
}

TEST(F38HyPEEnricherTest, NameIsHype) {
    HyPEEnricher e(HyPEConfig{}, std::make_shared<llm::MockLlmClient>(), nullptr);
    EXPECT_EQ(e.Name(), "hype");
}

TEST(F38HyPEEnricherTest, AvailableWithClient) {
    HyPEEnricher e(HyPEConfig{}, std::make_shared<llm::MockLlmClient>(), nullptr);
    EXPECT_TRUE(e.IsAvailable());
}

TEST(F38HyPEEnricherTest, UnavailableWithoutClient) {
    HyPEEnricher e(HyPEConfig{}, nullptr, nullptr);  // null client → degrade
    EXPECT_FALSE(e.IsAvailable());
}

TEST(F38HyPEEnricherTest, UnavailableWhenKBelowMin) {
    HyPEConfig cfg;
    cfg.questions_per_chunk = 0;  // below kHypeQuestionsMin
    HyPEEnricher e(cfg, std::make_shared<llm::MockLlmClient>(), nullptr);
    EXPECT_FALSE(e.IsAvailable());
}

// ---------- GenerateHypeQuestions ----------

TEST(F38HyPEEnricherTest, GeneratesKQuestions) {
    auto llm = std::make_shared<llm::MockLlmClient>();
    EXPECT_CALL(*llm, Chat(_, _))
        .WillOnce(Return(OkChat("What was Q1 revenue?\n"
                                "What was the YoY growth?\n"
                                "Why did revenue grow?")));
    HyPEConfig cfg;  // K=3
    HyPEEnricher e(cfg, llm, nullptr);

    auto r = e.GenerateHypeQuestions("Q1 revenue 520M, +23% YoY", /*parent=*/"",
                                     DocMeta(), "child_1", "parent_1");
    ASSERT_TRUE(r.ok()) << r.status().message();
    ASSERT_EQ(r.value().size(), 3u);
    EXPECT_EQ(r.value()[0].question_text, "What was Q1 revenue?");
    EXPECT_EQ(r.value()[0].question_index, 0);
    EXPECT_EQ(r.value()[1].question_index, 1);
    EXPECT_EQ(r.value()[0].source_child_id, "child_1");
    EXPECT_EQ(r.value()[0].source_parent_id, "parent_1");
    EXPECT_TRUE(r.value()[0].embedding.empty());  // filled by pipeline at S3
}

TEST(F38HyPEEnricherTest, GenerateLlmFailureReturnsError) {
    auto llm = std::make_shared<llm::MockLlmClient>();
    llm::ChatCompletionResponse fail;
    fail.status = Status::Unavailable("connect refused");
    EXPECT_CALL(*llm, Chat(_, _)).WillOnce(Return(fail));
    HyPEEnricher e(HyPEConfig{}, llm, nullptr);

    auto r = e.GenerateHypeQuestions("chunk", "", DocMeta(), "c", "p");
    EXPECT_FALSE(r.ok());
    EXPECT_NE(r.status().message().find("CX_ERR_F38_LLM_TIMEOUT"),
              std::string::npos);
}

TEST(F38HyPEEnricherTest, GenerateNullClientReturnsError) {
    HyPEEnricher e(HyPEConfig{}, nullptr, nullptr);
    auto r = e.GenerateHypeQuestions("chunk", "", DocMeta(), "c", "p");
    EXPECT_FALSE(r.ok());
}

TEST(F38HyPEEnricherTest, PromptIncludesParentTextWhenProvided) {
    // Capture the prompt to confirm parent_text context is threaded in (reconcile 2).
    auto llm = std::make_shared<llm::MockLlmClient>();
    std::string captured;
    EXPECT_CALL(*llm, Chat(_, _))
        .WillOnce([&](const std::string& prompt, const llm::LlmCallConfig&) {
            captured = prompt;
            return OkChat("q1\nq2\nq3");
        });
    HyPEEnricher e(HyPEConfig{}, llm, nullptr);
    (void)e.GenerateHypeQuestions("chunk body", "PARENT CONTEXT HERE", DocMeta(),
                                  "c", "p");
    EXPECT_NE(captured.find("PARENT CONTEXT HERE"), std::string::npos);
    EXPECT_NE(captured.find("chunk body"), std::string::npos);
}

// ---------- ResolveParentText (optional context, reconcile 2) ----------

TEST(F38HyPEEnricherTest, ResolveParentTextWithStore) {
    auto store = std::make_shared<store::MockParentChunkStore>();
    EXPECT_CALL(*store, GetParent("p1"))
        .WillOnce(Return(Result<cortrix::chunker::ParentChunk>(
            MakeParent("the full parent context"))));
    HyPEEnricher e(HyPEConfig{}, std::make_shared<llm::MockLlmClient>(), store);

    auto r = e.ResolveParentText("p1");
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value(), "the full parent context");
}

TEST(F38HyPEEnricherTest, ResolveParentTextNullStoreIsEmptyNotError) {
    HyPEEnricher e(HyPEConfig{}, std::make_shared<llm::MockLlmClient>(), nullptr);
    auto r = e.ResolveParentText("p1");
    ASSERT_TRUE(r.ok());            // optional context: no store → empty, not error
    EXPECT_TRUE(r.value().empty());
}

TEST(F38HyPEEnricherTest, ResolveParentTextEmptyIdIsEmptyNotError) {
    auto store = std::make_shared<store::MockParentChunkStore>();
    EXPECT_CALL(*store, GetParent(_)).Times(0);  // not called for empty id
    HyPEEnricher e(HyPEConfig{}, std::make_shared<llm::MockLlmClient>(), store);
    auto r = e.ResolveParentText("");
    ASSERT_TRUE(r.ok());
    EXPECT_TRUE(r.value().empty());
}

TEST(F38HyPEEnricherTest, ResolveParentTextNotFoundSurfacesError) {
    auto store = std::make_shared<store::MockParentChunkStore>();
    EXPECT_CALL(*store, GetParent("missing"))
        .WillOnce(Return(Result<cortrix::chunker::ParentChunk>(
            store::StoreStatus(StatusCode::kNotFound,
                               store::store_errors::kNotFound,
                               "parent_id absent"))));
    HyPEEnricher e(HyPEConfig{}, std::make_shared<llm::MockLlmClient>(), store);

    auto r = e.ResolveParentText("missing");
    EXPECT_FALSE(r.ok());
    EXPECT_NE(r.status().message().find("CX_ERR_F38_PARENT_NOT_FOUND"),
              std::string::npos);
}

// ---------- Enrich / EnrichBatch (standard EnrichResult, reconcile 1) ----------

TEST(F38HyPEEnricherTest, EnrichReturnsStandardResultSuccess) {
    auto llm = std::make_shared<llm::MockLlmClient>();
    EXPECT_CALL(*llm, Chat(_, _)).WillOnce(Return(OkChat("q1\nq2\nq3")));
    HyPEEnricher e(HyPEConfig{}, llm, nullptr);

    EnrichResult res = e.Enrich("chunk", DocMeta(), ChunkContext{});
    EXPECT_TRUE(res.ok());                 // status 0
    EXPECT_EQ(res.enricher_name, "hype");
    EXPECT_TRUE(res.entities.empty());     // HyPE does no NER (reconcile 1)
    EXPECT_TRUE(res.summary.empty());
}

TEST(F38HyPEEnricherTest, EnrichDegradesOnLlmFailure) {
    auto llm = std::make_shared<llm::MockLlmClient>();
    llm::ChatCompletionResponse fail;
    fail.status = Status::Unavailable("boom");
    EXPECT_CALL(*llm, Chat(_, _)).WillOnce(Return(fail));
    HyPEEnricher e(HyPEConfig{}, llm, nullptr);

    EnrichResult res = e.Enrich("chunk", DocMeta(), ChunkContext{});
    EXPECT_FALSE(res.ok());                // non-zero status = failed (degrade)
    EXPECT_FALSE(res.error_msg.empty());
}

// ---------- S2: strict ParseQuestions (§6.2) ----------

TEST(F38HyPEParseTest, ExactCountSucceeds) {
    auto r = HyPEEnricher::ParseQuestions("q1\nq2\nq3", 3);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value().size(), 3u);
    EXPECT_EQ(r.value()[2], "q3");
}

TEST(F38HyPEParseTest, TrimsAndDropsBlankLines) {
    auto r = HyPEEnricher::ParseQuestions("  q1  \n\n   \n q2 \nq3\n", 3);
    ASSERT_TRUE(r.ok());
    ASSERT_EQ(r.value().size(), 3u);
    EXPECT_EQ(r.value()[0], "q1");  // trimmed
    EXPECT_EQ(r.value()[1], "q2");
}

TEST(F38HyPEParseTest, TooFewIsParseFailed) {
    auto r = HyPEEnricher::ParseQuestions("q1\nq2", 3);  // 2 != 3
    EXPECT_FALSE(r.ok());
    EXPECT_NE(r.status().message().find("CX_ERR_F38_QUESTION_PARSE_FAILED"),
              std::string::npos);
}

TEST(F38HyPEParseTest, TooManyIsParseFailed) {
    auto r = HyPEEnricher::ParseQuestions("q1\nq2\nq3\nq4", 3);  // 4 != 3
    EXPECT_FALSE(r.ok());
    EXPECT_NE(r.status().message().find("CX_ERR_F38_QUESTION_PARSE_FAILED"),
              std::string::npos);
}

TEST(F38HyPEParseTest, EmptyOutputIsInvalidOutput) {
    auto r = HyPEEnricher::ParseQuestions("   \n  \n", 3);
    EXPECT_FALSE(r.ok());
    EXPECT_NE(r.status().message().find("CX_ERR_F38_LLM_INVALID_OUTPUT"),
              std::string::npos);
}

TEST(F38HyPEEnricherTest, GenerateRejectsCountMismatch) {
    auto llm = std::make_shared<llm::MockLlmClient>();
    EXPECT_CALL(*llm, Chat(_, _)).WillOnce(Return(OkChat("only one")));  // 1 != 3
    HyPEEnricher e(HyPEConfig{}, llm, nullptr);
    auto r = e.GenerateHypeQuestions("chunk", "", DocMeta(), "c", "p");
    EXPECT_FALSE(r.ok());
    EXPECT_NE(r.status().message().find("CX_ERR_F38_QUESTION_PARSE_FAILED"),
              std::string::npos);
}

TEST(F38HyPEEnricherTest, GenerateRespectsConfiguredK) {
    auto llm = std::make_shared<llm::MockLlmClient>();
    EXPECT_CALL(*llm, Chat(_, _)).WillOnce(Return(OkChat("q1\nq2\nq3\nq4\nq5")));
    HyPEConfig cfg;
    cfg.questions_per_chunk = 5;  // K=5
    HyPEEnricher e(cfg, llm, nullptr);
    auto r = e.GenerateHypeQuestions("chunk", "", DocMeta(), "c", "p");
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value().size(), 5u);
}

// ---------- S2: parent_text omission in prompt (reconcile 2) ----------

TEST(F38HyPEEnricherTest, PromptOmitsParentSectionWhenEmpty) {
    HyPEEnricher e(HyPEConfig{}, std::make_shared<llm::MockLlmClient>(), nullptr);
    std::string with = e.BuildPrompt("chunk", "PCTX", DocMeta(), 3);
    std::string without = e.BuildPrompt("chunk", "", DocMeta(), 3);
    EXPECT_NE(with.find("Parent context"), std::string::npos);
    EXPECT_EQ(without.find("Parent context"), std::string::npos);  // omitted
    EXPECT_NE(without.find("Chunk:"), std::string::npos);          // chunk still there
    EXPECT_NE(with.find("exactly 3"), std::string::npos);
}

TEST(F38HyPEEnricherTest, PromptOmitsDocTitleWhenEmpty) {
    HyPEEnricher e(HyPEConfig{}, std::make_shared<llm::MockLlmClient>(), nullptr);
    DocumentMetadata no_title;  // doc_title empty
    std::string p = e.BuildPrompt("chunk", "", no_title, 3);
    EXPECT_EQ(p.find("Document:"), std::string::npos);
}

TEST(F38HyPEEnricherTest, EnrichBatchRunsPerContext) {
    auto llm = std::make_shared<llm::MockLlmClient>();
    EXPECT_CALL(*llm, Chat(_, _))
        .WillRepeatedly(Return(OkChat("q1\nq2\nq3")));
    HyPEEnricher e(HyPEConfig{}, llm, nullptr);

    DocumentMetadata meta = DocMeta();
    std::vector<ChunkContext> ctxs;
    for (int i = 0; i < 3; ++i) {
        ChunkContext c;
        c.chunk_text = "chunk " + std::to_string(i);
        c.chunk_index = i;
        c.doc_metadata = &meta;
        ctxs.push_back(c);
    }
    auto results = e.EnrichBatch(ctxs);
    ASSERT_EQ(results.size(), 3u);
    for (const auto& r : results) EXPECT_TRUE(r.ok());
}

}  // namespace
}  // namespace cortrix::spc
