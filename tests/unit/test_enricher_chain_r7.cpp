// R7 branch-coverage supplement for src/spc/enricher_chain.cpp (branch 53.3%).
//
// The existing test_enricher_chain.cpp covers the happy merge, the
// fail-soft throw catch, the unavailable soft-skip, and the hype happy path.
// The remaining dead branches in EnrichChunks() are the per-field merge-policy
// conditionals (each `if` is two branches; the existing tests only take the
// "populated"/"first-write" side) plus the degraded (non-throwing) error-merge arm
// and the HyPE question-parse-failure (qres NOT ok) arm:
//   - merged.enricher_name already set → later step does NOT overwrite (line ~186);
//   - token_count / duration_ms accumulation present vs absent (~188-189);
//   - model_used / prompt_version empty vs present (~190-191);
//   - contextualized_* has_value() FALSE arm (an enricher that returns none);
//   - the step.ok()==false (degraded, not thrown) error-merge: err_code from
//     structured_data vs error_msg, merged.status first-capture vs "later error
//     does not clear" (~201-213);
//   - the hype GenerateHypeQuestions FAILURE arm (wrong question count →
//     ParseQuestions fails → qres NOT ok → degraded step recorded, ~145-153);
//   - the provenance index-bounds false arms (empty parent_texts/src ids, ~129-134).
//
// All via local fake ISpcEnricher subclasses + a real HyPEEnricher driven by a
// MockLlmClient returning a WRONG question count. Standalone NEW file; does not
// touch the existing test_enricher_chain.cpp.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "cortrix/spc/enricher_chain.h"
#include "cortrix/spc/hype_enricher.h"
#include "mock_llm_client.h"

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

// F03-style enricher that stamps the full set of merge-source fields (entities +
// summary + score + enricher_name + token_count + duration_ms + model_used +
// prompt_version). Drives the "first write" side of every merge conditional.
class FullFieldsEnricher : public ISpcEnricher {
public:
    EnrichResult Enrich(const std::string&, const DocumentMetadata&,
                        const ChunkContext&) override {
        EnrichResult r;
        r.summary = "first summary";
        r.enricher_name = "LlmEnricher";
        r.enriched_score = 0.7f;
        r.token_count = 10;
        r.duration_ms = 5;
        r.model_used = "model-a";
        r.prompt_version = "v1";
        Entity e; e.text = "Cortrix"; e.type = "PRODUCT";
        r.entities.push_back(e);
        return r;
    }
    std::vector<EnrichResult> EnrichBatch(const std::vector<ChunkContext>& c) override {
        std::vector<EnrichResult> out;
        for (const auto& ctx : c) out.push_back(Enrich(ctx.chunk_text, *ctx.doc_metadata, ctx));
        return out;
    }
    bool IsAvailable() const override { return true; }
    std::string Name() const override { return "LlmEnricher"; }
};

// A SECOND F03-style enricher that ALSO stamps every field — when chained after
// FullFieldsEnricher it exercises the "already populated, do not overwrite" FALSE
// arms (enricher_name kept; entities/summary replaced only because non-empty; the
// accumulation arms for token_count/duration_ms).
class SecondFullEnricher : public ISpcEnricher {
public:
    EnrichResult Enrich(const std::string&, const DocumentMetadata&,
                        const ChunkContext&) override {
        EnrichResult r;
        r.summary = "second summary";
        r.enricher_name = "SecondEnricher";  // merged keeps the FIRST name (false arm)
        r.enriched_score = 0.9f;
        r.token_count = 3;   // accumulates onto the first
        r.duration_ms = 2;   // accumulates onto the first
        r.model_used = "model-b";
        r.prompt_version = "v2";
        return r;
    }
    std::vector<EnrichResult> EnrichBatch(const std::vector<ChunkContext>& c) override {
        std::vector<EnrichResult> out;
        for (const auto& ctx : c) out.push_back(Enrich(ctx.chunk_text, *ctx.doc_metadata, ctx));
        return out;
    }
    bool IsAvailable() const override { return true; }
    std::string Name() const override { return "SecondEnricher"; }
};

// An enricher that returns an EMPTY EnrichResult (all fields default/unset) — drives
// the FALSE arm of every "if (!step.X.empty())" / "if (step.X > 0)" / has_value()
// merge guard (nothing is merged from it).
class EmptyResultEnricher : public ISpcEnricher {
public:
    EnrichResult Enrich(const std::string&, const DocumentMetadata&,
                        const ChunkContext&) override {
        return EnrichResult{};  // status 0, no fields set
    }
    std::vector<EnrichResult> EnrichBatch(const std::vector<ChunkContext>& c) override {
        return std::vector<EnrichResult>(c.size());  // one default result per context
    }
    bool IsAvailable() const override { return true; }
    std::string Name() const override { return "EmptyEnricher"; }
};

// A non-throwing enricher that returns a DEGRADED result (status != 0) with an
// error_msg but no structured_data → drives the step.ok()==false error-merge arm
// taking the error_msg branch (structured_data empty) + merged.status first-capture.
class DegradedMsgEnricher : public ISpcEnricher {
public:
    EnrichResult Enrich(const std::string&, const DocumentMetadata&,
                        const ChunkContext&) override {
        EnrichResult r;
        r.status = 7;                 // non-zero → degraded (not thrown)
        r.error_msg = "degraded-a";   // structured_data left empty → err_code = error_msg
        return r;
    }
    std::vector<EnrichResult> EnrichBatch(const std::vector<ChunkContext>& c) override {
        std::vector<EnrichResult> out;
        for (const auto& ctx : c) out.push_back(Enrich(ctx.chunk_text, *ctx.doc_metadata, ctx));
        return out;
    }
    bool IsAvailable() const override { return true; }
    std::string Name() const override { return "DegradedMsg"; }
};

// A non-throwing enricher that returns a DEGRADED result with structured_data set →
// drives the err_code = structured_data branch (the other side of the ternary). Its
// status must NOT clobber an already-captured merged.status (the "later error does
// not clear" arm) when chained after DegradedMsgEnricher.
class DegradedStructuredEnricher : public ISpcEnricher {
public:
    EnrichResult Enrich(const std::string&, const DocumentMetadata&,
                        const ChunkContext&) override {
        EnrichResult r;
        r.status = 9;
        r.error_msg = "degraded-b";
        r.error_meta.structured_data = R"({"k":"v"})";  // non-empty → err_code = this
        return r;
    }
    std::vector<EnrichResult> EnrichBatch(const std::vector<ChunkContext>& c) override {
        std::vector<EnrichResult> out;
        for (const auto& ctx : c) out.push_back(Enrich(ctx.chunk_text, *ctx.doc_metadata, ctx));
        return out;
    }
    bool IsAvailable() const override { return true; }
    std::string Name() const override { return "DegradedStructured"; }
};

// ---------- merge-policy false arms ----------

// Two full-field enrichers in a row: the second's fields hit the "already set"
// false arms — merged.enricher_name stays the FIRST one; token_count/duration_ms
// ACCUMULATE; summary/model/prompt are overwritten (non-empty later value wins by
// design). Confirms the merge conditionals' alternate branches.
TEST(EnricherChainR7, SecondEnricherHitsAlreadySetFalseArms) {
    auto dm = DocMeta();
    EnricherChain chain;
    chain.Append(std::make_shared<FullFieldsEnricher>());
    chain.Append(std::make_shared<SecondFullEnricher>());

    auto ctxs = MakeContexts({"c1"}, &dm);
    auto res = chain.EnrichChunks(ctxs, {}, {}, {});
    ASSERT_EQ(res.size(), 1u);
    const EnrichResult& m = res[0].merged;
    // enricher_name: first one kept (merged.enricher_name already non-empty → false arm).
    EXPECT_EQ(m.enricher_name, "LlmEnricher");
    // token_count / duration_ms accumulated across both steps.
    EXPECT_EQ(m.token_count, 13);  // 10 + 3
    EXPECT_EQ(m.duration_ms, 7);   // 5 + 2
    // summary / model_used / prompt_version: later non-empty value wins.
    EXPECT_EQ(m.summary, "second summary");
    EXPECT_EQ(m.model_used, "model-b");
    EXPECT_EQ(m.prompt_version, "v2");
    // both steps ok.
    EXPECT_EQ(m.status, 0);
}

// A full enricher followed by an EMPTY-result enricher: every "if (!step.X.empty())"
// / "if (step.X > 0)" / has_value() guard takes its FALSE arm for the empty step, so
// the merged result is exactly the first enricher's output (nothing overwritten, no
// accumulation, no contextualized fields).
TEST(EnricherChainR7, EmptyResultEnricherMergesNothing) {
    auto dm = DocMeta();
    EnricherChain chain;
    chain.Append(std::make_shared<FullFieldsEnricher>());
    chain.Append(std::make_shared<EmptyResultEnricher>());

    auto ctxs = MakeContexts({"c1"}, &dm);
    auto res = chain.EnrichChunks(ctxs, {}, {}, {});
    ASSERT_EQ(res.size(), 1u);
    const EnrichResult& m = res[0].merged;
    EXPECT_EQ(m.summary, "first summary");      // unchanged
    EXPECT_EQ(m.enriched_score, 0.7f);          // unchanged (empty step had 0)
    EXPECT_EQ(m.token_count, 10);               // no accumulation
    EXPECT_EQ(m.duration_ms, 5);
    EXPECT_EQ(m.model_used, "model-a");
    EXPECT_FALSE(m.contextualized_text.has_value());        // Contextual retrieval has_value() false arm
    EXPECT_FALSE(m.contextualized_embedding.has_value());
    EXPECT_EQ(m.contextualized_status, 0);
    EXPECT_EQ(res[0].steps.size(), 2u);
}

// ---------- degraded (non-throwing) error-merge arms ----------

// A degraded (status!=0) enricher with only error_msg → the error-merge takes the
// err_code = error_msg branch (structured_data empty) AND captures merged.status
// (was 0). Distinct from the THROW path (which the existing test covers).
TEST(EnricherChainR7, DegradedStatusCapturedFromErrorMsg) {
    auto dm = DocMeta();
    EnricherChain chain;
    chain.Append(std::make_shared<DegradedMsgEnricher>());

    auto ctxs = MakeContexts({"c1"}, &dm);
    auto res = chain.EnrichChunks(ctxs, {}, {}, {});
    ASSERT_EQ(res.size(), 1u);
    EXPECT_EQ(res[0].merged.status, 7);  // first-error capture
    ASSERT_EQ(res[0].steps.size(), 1u);
    EXPECT_EQ(res[0].steps[0].status, 7);
    EXPECT_EQ(res[0].steps[0].error_code, "degraded-a");  // from error_msg
}

// Two degraded enrichers: the FIRST (error_msg) captures merged.status; the SECOND
// (structured_data) records its own step err_code from structured_data BUT must NOT
// overwrite the already-captured merged.status (the "if merged.status == 0" false
// arm — later error does not clear). Covers both ternary sides + the no-clobber arm.
TEST(EnricherChainR7, SecondDegradedDoesNotClobberStatus_StructuredErrCode) {
    auto dm = DocMeta();
    EnricherChain chain;
    chain.Append(std::make_shared<DegradedMsgEnricher>());         // captures status=7
    chain.Append(std::make_shared<DegradedStructuredEnricher>());  // status=9, struct data

    auto ctxs = MakeContexts({"c1"}, &dm);
    auto res = chain.EnrichChunks(ctxs, {}, {}, {});
    ASSERT_EQ(res.size(), 1u);
    EXPECT_EQ(res[0].merged.status, 7) << "first error is kept; later error must not clear it";
    ASSERT_EQ(res[0].steps.size(), 2u);
    EXPECT_EQ(res[0].steps[1].status, 9);
    EXPECT_EQ(res[0].steps[1].error_code, R"({"k":"v"})");  // from structured_data branch
}

// ---------- hype: GenerateHypeQuestions FAILURE (qres NOT ok) --------------

// A HyPEEnricher whose MockLlmClient returns the WRONG number of questions (1, not
// the default K=3) → ParseQuestions fails → GenerateHypeQuestions returns non-OK →
// the chain records a DEGRADED hype step (status != 0) and produces NO questions.
// This is the qres.ok()==false arm in the hype side channel (the existing test only
// covers the success arm).
TEST(EnricherChainR7, HypeParseFailureRecordsDegradedStep) {
    auto llm = std::make_shared<llm::MockLlmClient>();
    llm::ChatCompletionResponse bad;
    bad.status = Status::Ok();
    bad.model = "gpt-4o-mini";
    bad.content = "only one question?";  // 1 line, but K defaults to 3 → parse failure
    EXPECT_CALL(*llm, Chat(_, _)).WillRepeatedly(Return(bad));

    EnricherChain chain;
    chain.Append(std::make_shared<HyPEEnricher>(HyPEConfig{}, llm, nullptr));
    ASSERT_TRUE(chain.AnyAvailable());

    auto dm = DocMeta();
    auto ctxs = MakeContexts({"the chunk"}, &dm);
    auto res = chain.EnrichChunks(ctxs, {"parent ctx"}, {"child1"}, {"parent1"});
    ASSERT_EQ(res.size(), 1u);
    EXPECT_TRUE(res[0].hype_questions.empty()) << "parse failure → no questions";
    // a hype step recorded with a non-zero status (degraded, not skipped).
    bool degraded = false;
    for (const auto& s : res[0].steps) {
        if (s.name == "hype" && !s.skipped && s.status != 0) degraded = true;
    }
    EXPECT_TRUE(degraded);
}

// Hype with EMPTY provenance vectors (parent_texts / source_child_ids /
// source_parent_ids all empty) → the index-bounds guards (i < X.size()) take their
// FALSE arms, so parent_text / child_id / parent_id default to "" — questions still
// generated, provenance blank. Covers the alternate side of those three guards.
TEST(EnricherChainR7, HypeEmptyProvenanceVectorsUseDefaults) {
    auto llm = std::make_shared<llm::MockLlmClient>();
    llm::ChatCompletionResponse ok;
    ok.status = Status::Ok();
    ok.model = "gpt-4o-mini";
    ok.content = "Q one?\nQ two?\nQ three?";  // exactly K=3
    EXPECT_CALL(*llm, Chat(_, _)).WillRepeatedly(Return(ok));

    EnricherChain chain;
    chain.Append(std::make_shared<HyPEEnricher>(HyPEConfig{}, llm, nullptr));

    auto dm = DocMeta();
    auto ctxs = MakeContexts({"the chunk"}, &dm);
    // Pass EMPTY provenance vectors → exercises the false arms of the 3 bounds checks.
    auto res = chain.EnrichChunks(ctxs, {}, {}, {});
    ASSERT_EQ(res.size(), 1u);
    ASSERT_EQ(res[0].hype_questions.size(), 3u);
    EXPECT_EQ(res[0].hype_questions[0].source_child_id, "");   // default (no provenance)
    EXPECT_EQ(res[0].hype_questions[0].source_parent_id, "");
}

// A chain with a single null enricher Append (ignored) + an unavailable member only:
// AnyAvailable() is false and EnrichChunks returns one default result per context
// with only the soft-skip step. Covers Append(nullptr) ignore + the all-unavailable
// EnrichChunks shape.
TEST(EnricherChainR7, NullAppendIgnored_AllUnavailableYieldsDefaults) {
    EnricherChain chain;
    chain.Append(nullptr);  // ignored (Append null-guard)
    chain.Append(std::make_shared<HyPEEnricher>(HyPEConfig{}, nullptr, nullptr));  // unavailable
    EXPECT_FALSE(chain.AnyAvailable());
    EXPECT_EQ(chain.Names().size(), 1u);  // only the hype enricher is named (null ignored)

    auto dm = DocMeta();
    auto ctxs = MakeContexts({"c1", "c2"}, &dm);
    auto res = chain.EnrichChunks(ctxs, {}, {}, {});
    ASSERT_EQ(res.size(), 2u);
    for (const auto& r : res) {
        EXPECT_TRUE(r.merged.summary.empty());
        EXPECT_TRUE(r.hype_questions.empty());
        // the unavailable hype member recorded a skipped step on each chunk.
        ASSERT_EQ(r.steps.size(), 1u);
        EXPECT_TRUE(r.steps[0].skipped);
    }
}

}  // namespace
}  // namespace cortrix::spc
