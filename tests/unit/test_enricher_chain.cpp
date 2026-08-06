#include <gtest/gtest.h>

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "cortrix/common/in_memory_global_config.h"
#include "cortrix/spc/enricher_chain.h"
#include "cortrix/spc/hype_enricher.h"
#include "mock_llm_client.h"

// I1 — ISpcEnricher chain framework (enricher → contextual retrieval → HyPE fail-soft serial). Tests the
// chain-spec parsing, the NS/GUC resolution, the per-chunk merge across enrichers,
// the fail-soft skip on throw, and the hype side channel.
namespace cortrix::spc {
namespace {

using ::testing::_;
using ::testing::Return;

DocumentMetadata DocMeta() {
    DocumentMetadata m;
    m.doc_title = "Doc";
    return m;
}

std::vector<ChunkContext> MakeContexts(const std::vector<std::string>& texts,
                                       const DocumentMetadata* dm) {
    std::vector<ChunkContext> ctxs;
    for (size_t i = 0; i < texts.size(); ++i) {
        ChunkContext c;
        c.chunk_text = texts[i];
        c.chunk_index = static_cast<int>(i);
        c.doc_metadata = dm;
        ctxs.push_back(std::move(c));
    }
    return ctxs;
}

// A fake enricher-style enricher: stamps summary + a name onto each result.
class FakeSummaryEnricher : public ISpcEnricher {
public:
    explicit FakeSummaryEnricher(std::string summary) : summary_(std::move(summary)) {}
    EnrichResult Enrich(const std::string&, const DocumentMetadata&,
                        const ChunkContext&) override {
        EnrichResult r;
        r.summary = summary_;
        r.enricher_name = "FakeSummary";
        r.enriched_score = 0.5f;
        return r;
    }
    std::vector<EnrichResult> EnrichBatch(const std::vector<ChunkContext>& c) override {
        std::vector<EnrichResult> out;
        for (size_t i = 0; i < c.size(); ++i)
            out.push_back(Enrich(c[i].chunk_text, *c[i].doc_metadata, c[i]));
        return out;
    }
    bool IsAvailable() const override { return true; }
    std::string Name() const override { return "FakeSummary"; }

private:
    std::string summary_;
};

// A fake contextual-style enricher: stamps contextualized_* onto each result.
class FakeContextualEnricher : public ISpcEnricher {
public:
    EnrichResult Enrich(const std::string& text, const DocumentMetadata&,
                        const ChunkContext&) override {
        EnrichResult r;
        r.contextualized_text = "ctx: " + text;
        r.contextualized_embedding = std::vector<float>{0.1f, 0.2f, 0.3f};
        r.contextualized_status = 1;  // generated
        return r;
    }
    std::vector<EnrichResult> EnrichBatch(const std::vector<ChunkContext>& c) override {
        std::vector<EnrichResult> out;
        for (size_t i = 0; i < c.size(); ++i)
            out.push_back(Enrich(c[i].chunk_text, *c[i].doc_metadata, c[i]));
        return out;
    }
    bool IsAvailable() const override { return true; }
    std::string Name() const override { return "contextual_retrieval"; }
};

// A fake contextual-style enricher whose member FAILS while the step stays OK
// (status 0, contextualized_status 2, error_msg set) — the real contextual retrieval degrade
// shape (§7.3 transparent degrade / §8 injection-guard reject).
class FailSoftContextualEnricher : public ISpcEnricher {
public:
    EnrichResult Enrich(const std::string&, const DocumentMetadata&,
                        const ChunkContext&) override {
        EnrichResult r;
        r.contextualized_status = 2;  // failed (fail-soft: step status stays 0)
        r.error_msg = "output length 300 exceeds 2x max_output_tokens 80";
        r.error_meta.structured_data =
            "{\"enricher\":\"contextual_retrieval\"}";
        return r;
    }
    std::vector<EnrichResult> EnrichBatch(const std::vector<ChunkContext>& c) override {
        std::vector<EnrichResult> out;
        for (size_t i = 0; i < c.size(); ++i)
            out.push_back(Enrich(c[i].chunk_text, *c[i].doc_metadata, c[i]));
        return out;
    }
    bool IsAvailable() const override { return true; }
    std::string Name() const override { return "contextual_retrieval"; }
};

// A fake enricher that always throws (exercises the fail-soft catch).
class ThrowingEnricher : public ISpcEnricher {
public:
    EnrichResult Enrich(const std::string&, const DocumentMetadata&,
                        const ChunkContext&) override {
        throw std::runtime_error("boom");
    }
    std::vector<EnrichResult> EnrichBatch(const std::vector<ChunkContext>&) override {
        throw std::runtime_error("boom");
    }
    bool IsAvailable() const override { return true; }
    std::string Name() const override { return "thrower"; }
};

// ---------- ParseEnricherChainSpec ----------

TEST(EnricherChainSpec, EmptyDefaultsToF03) {
    EXPECT_EQ(ParseEnricherChainSpec(""), (std::vector<std::string>{"enrich"}));
}

TEST(EnricherChainSpec, FullChainOrdered) {
    EXPECT_EQ(ParseEnricherChainSpec("enrich,contextual,hype"),
              (std::vector<std::string>{"enrich", "contextual", "hype"}));
}

TEST(EnricherChainSpec, EnricherAlwaysLeads) {
    // contextual alone implies enrich first (GS-2: enricher leads).
    EXPECT_EQ(ParseEnricherChainSpec("contextual"), (std::vector<std::string>{"enrich", "contextual"}));
    // enrich mentioned out of order is moved to the front.
    EXPECT_EQ(ParseEnricherChainSpec("hype,enrich"),
              (std::vector<std::string>{"enrich", "hype"}));
}

TEST(EnricherChainSpec, UnknownTokensDropped) {
    EXPECT_EQ(ParseEnricherChainSpec("enrich, bogus ,hype"),
              (std::vector<std::string>{"enrich", "hype"}));
}

TEST(EnricherChainSpec, DedupAndTrimAndCase) {
    EXPECT_EQ(ParseEnricherChainSpec(" enricher , contextual , contextual "),
              (std::vector<std::string>{"enrich", "contextual"}));
}

// ---------- ResolveEnricherChain ----------

TEST(EnricherChainResolve, NsOverrideWins) {
    InMemoryGlobalConfig g;
    g.Set("enricher.chain", "enrich");
    auto out = ResolveEnricherChain(&g, R"({"enricher_chain":"enrich,contextual,hype"})");
    EXPECT_EQ(out, (std::vector<std::string>{"enrich", "contextual", "hype"}));
}

TEST(EnricherChainResolve, GucUsedWhenNoNsOverride) {
    InMemoryGlobalConfig g;
    g.Set("enricher.chain", "enrich,contextual");
    auto out = ResolveEnricherChain(&g, "");
    EXPECT_EQ(out, (std::vector<std::string>{"enrich", "contextual"}));
}

TEST(EnricherChainResolve, DefaultWhenNothingSet) {
    EXPECT_EQ(ResolveEnricherChain(nullptr, ""), (std::vector<std::string>{"enrich"}));
}

// ---------- EnrichChunks (merge + fail-soft) ----------

TEST(EnricherChainRun, MergesEnricherAndF35PerChunk) {
    auto dm = DocMeta();
    EnricherChain chain;
    chain.Append(std::make_shared<FakeSummaryEnricher>("a summary"));
    chain.Append(std::make_shared<FakeContextualEnricher>());

    auto ctxs = MakeContexts({"chunk one", "chunk two"}, &dm);
    auto res = chain.EnrichChunks(ctxs, {}, {}, {});
    ASSERT_EQ(res.size(), 2u);
    for (size_t i = 0; i < res.size(); ++i) {
        EXPECT_EQ(res[i].merged.summary, "a summary");
        ASSERT_TRUE(res[i].merged.contextualized_text.has_value());
        EXPECT_EQ(res[i].merged.contextualized_status, 1);
        ASSERT_TRUE(res[i].merged.contextualized_embedding.has_value());
        EXPECT_EQ(res[i].merged.contextualized_embedding->size(), 3u);
        // both steps recorded, neither skipped
        EXPECT_EQ(res[i].steps.size(), 2u);
    }
}

// D12 (2026-07-11): the contextual retrieval fail-soft outcome (step OK, contextualized_status 2)
// must carry its error_msg into the step record — otherwise the debt row is
// written with a blank last_error and the real cause (live 5k ingest: the §8
// injection-guard byte limit, 4,139 blank rows) is unobservable.
TEST(EnricherChainRun, ContextualFailSoftCarriesErrorCodeInStepRecord) {
    auto dm = DocMeta();
    EnricherChain chain;
    chain.Append(std::make_shared<FakeSummaryEnricher>("kept"));
    chain.Append(std::make_shared<FailSoftContextualEnricher>());

    auto ctxs = MakeContexts({"c1"}, &dm);
    auto res = chain.EnrichChunks(ctxs, {}, {}, {});
    ASSERT_EQ(res.size(), 1u);
    // The member failure is recorded in the merged result...
    EXPECT_EQ(res[0].merged.contextualized_status, 2);
    // ...the step itself stays OK (fail-soft: chunk keeps original embedding)...
    const EnricherStepOutcome* contextual = nullptr;
    for (const auto& s : res[0].steps)
        if (s.name == "contextual_retrieval") contextual = &s;
    ASSERT_NE(contextual, nullptr);
    EXPECT_EQ(contextual->status, 0);
    EXPECT_FALSE(contextual->skipped);
    // ...but the cause is preserved for the debt-row writers (the fix).
    EXPECT_EQ(contextual->error_code, "output length 300 exceeds 2x max_output_tokens 80");
    // Enricher output unaffected by the contextual retrieval member failure.
    EXPECT_EQ(res[0].merged.summary, "kept");
}

TEST(EnricherChainRun, FailSoftSkipsThrowingEnricher) {
    auto dm = DocMeta();
    EnricherChain chain;
    chain.Append(std::make_shared<FakeSummaryEnricher>("kept"));
    chain.Append(std::make_shared<ThrowingEnricher>());
    chain.Append(std::make_shared<FakeContextualEnricher>());

    auto ctxs = MakeContexts({"c1"}, &dm);
    auto res = chain.EnrichChunks(ctxs, {}, {}, {});
    ASSERT_EQ(res.size(), 1u);
    // Enricher summary kept AND contextualized still applied despite the throw between.
    EXPECT_EQ(res[0].merged.summary, "kept");
    EXPECT_TRUE(res[0].merged.contextualized_text.has_value());
    // the thrower's step is recorded as a non-zero status.
    bool found_thrower = false;
    for (const auto& s : res[0].steps) {
        if (s.name == "thrower") {
            found_thrower = true;
            EXPECT_NE(s.status, 0);
        }
    }
    EXPECT_TRUE(found_thrower);
}

TEST(EnricherChainRun, SkipsUnavailableMembers) {
    EnricherChain chain;
    // An LLM-less HyPE enricher is unavailable (null client) → soft skip.
    chain.Append(std::make_shared<FakeSummaryEnricher>("s"));
    chain.Append(std::make_shared<HyPEEnricher>(HyPEConfig{},
                                                /*llm=*/nullptr, /*store=*/nullptr));
    EXPECT_TRUE(chain.AnyAvailable());  // the summary fake is available

    auto dm = DocMeta();
    auto ctxs = MakeContexts({"c1"}, &dm);
    auto res = chain.EnrichChunks(ctxs, {""}, {"child1"}, {"parent1"});
    ASSERT_EQ(res.size(), 1u);
    EXPECT_EQ(res[0].merged.summary, "s");
    EXPECT_TRUE(res[0].hype_questions.empty());  // hype unavailable → no questions
    // the hype step is recorded as skipped.
    bool hype_skipped = false;
    for (const auto& s : res[0].steps)
        if (s.name == "hype" && s.skipped) hype_skipped = true;
    EXPECT_TRUE(hype_skipped);
}

TEST(EnricherChainRun, HypeSideChannelProducesQuestions) {
    auto llm = std::make_shared<llm::MockLlmClient>();
    llm::ChatCompletionResponse ok;
    ok.content = "Q one?\nQ two?\nQ three?";
    ok.model = "gpt-4o-mini";
    EXPECT_CALL(*llm, Chat(_, _)).WillRepeatedly(Return(ok));

    EnricherChain chain;
    chain.Append(std::make_shared<HyPEEnricher>(HyPEConfig{}, llm, nullptr));
    ASSERT_TRUE(chain.AnyAvailable());

    auto dm = DocMeta();
    auto ctxs = MakeContexts({"the chunk"}, &dm);
    auto res = chain.EnrichChunks(ctxs, {"parent ctx"}, {"child1"}, {"parent1"});
    ASSERT_EQ(res.size(), 1u);
    ASSERT_EQ(res[0].hype_questions.size(), 3u);
    EXPECT_EQ(res[0].hype_questions[0].source_child_id, "child1");
    EXPECT_EQ(res[0].hype_questions[0].source_parent_id, "parent1");
}

}  // namespace
}  // namespace cortrix::spc
