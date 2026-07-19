// test_query_module_arms.cpp — residual line-gate arms for the query module
// (push query line coverage past the 90% gate). Each case below pins a specific
// uncovered arm from QUERY90_GAPS.md that the existing query tests
// (test_intent_classifier / test_query_pipeline / test_wordpiece_* /
// test_onnx_complexity_backend / test_query_coverage_gaps) do NOT already hit.
//
// Deterministic only: in-process fakes, no live LLM / network / sleeps. The ONNX
// backend cases use the LOCAL query-complexity model + vocab next to it (not a
// network fetch); they GTEST_SKIP when the model archive is absent (offline build),
// matching the in-tree ONNX test convention.
//
// gtest suite names are GLOBAL — every suite here uses a module-unique "*Arms"
// prefix (WordpieceArms / IntentClassifierArms / QueryPipelineArms /
// OnnxComplexityArms) so it never clashes with the existing query suites.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "cortrix/common/data_types.h"
#include "cortrix/config/config.h"
#include "cortrix/query/bm25_searcher.h"
#include "cortrix/query/degradation_manager.h"
#include "cortrix/query/intent_classifier.h"
#include "cortrix/query/onnx_complexity_backend.h"
#include "cortrix/query/post_filter.h"
#include "cortrix/query/query_pipeline.h"
#include "cortrix/query/query_request.h"
#include "cortrix/query/rrf_fusion.h"
#include "cortrix/query/router_error.h"
#include "cortrix/query/sql_executor.h"
#include "cortrix/query/vector_searcher.h"
#include "cortrix/retrieval/classifier.h"
#include "cortrix/spc/onnx_embedder.h"

#include "ns_pool_test_helper.h"  // cortrix::test::FakeIndex (IIndex fake)

namespace fs = std::filesystem;

// ===========================================================================
// IntentClassifierArms — the defensive default arm in IntentToLabel.
//
// Gap (intent_classifier.cpp line 31): `return "semantic";` after the switch over
// QueryIntent. The switch covers all three valid enum values, so a *valid* enum
// never reaches the fall-through. IntentToLabel is a PUBLIC static, so we can hit
// the defensive default by passing an out-of-range cast value (what would happen
// if a future enum member is added but not mapped) — a legitimate deterministic
// guard on the unreachable-default contract.
//
// NOTE on lines 17-21 (the LLM try/catch fallback in the 2-arg Classify): these are
// NOT reachable as black-box tests. ClassifyByLlm is an MVP stub that always returns
// ClassifyByKeyword(...) and never throws, so the catch(...) arm cannot fire without
// modifying src/. Documented, not covered.
// ===========================================================================
namespace cortrix {
namespace {

TEST(IntentClassifierArms, IntentToLabelOutOfRangeFallsBackToSemantic) {
    // Out-of-range enum value (not kSemantic/kSql/kHybrid) → switch falls through to
    // the defensive `return "semantic";` (intent_classifier.cpp line 31).
    const QueryIntent bogus = static_cast<QueryIntent>(99);
    EXPECT_STREQ(IntentClassifier::IntentToLabel(bogus), "semantic");
}

// Belt-and-suspenders: every *valid* enum still maps to its own label (so the test
// above is genuinely exercising only the fall-through, not masking a switch bug).
TEST(IntentClassifierArms, IntentToLabelValidEnumsUnchanged) {
    EXPECT_STREQ(IntentClassifier::IntentToLabel(QueryIntent::kSemantic), "semantic");
    EXPECT_STREQ(IntentClassifier::IntentToLabel(QueryIntent::kSql), "sql");
    EXPECT_STREQ(IntentClassifier::IntentToLabel(QueryIntent::kHybrid), "hybrid");
}

// ===========================================================================
// QueryPipelineArms — the doc_id fast-path branch (query_pipeline.cpp 34-49).
//
// Every existing QueryPipeline Execute test leaves request.filters.doc_id empty, so
// the `if (!request.filters.doc_id.empty())` fast-path (FetchByDocId, bypassing
// HNSW/BM25, returning chunk-ordered blocks) is never entered. We drive it with a
// store that actually returns blocks for the doc so the branch executes end-to-end
// and assembles the doc_id response (routes_used == {"doc_id"}, not degraded).
// ===========================================================================

// Fake store that serves block_get_by_doc / doc_get so FetchByDocId yields rows.
class DocIdFakeStore : public CortrixStore {
public:
    std::unordered_map<int64_t, CortrixBlock> blocks;
    std::unordered_map<std::string, CortrixDoc> docs;

    int Open() override { return 0; }
    int Close() override { return 0; }
    int doc_create(CortrixDoc&) override { return 0; }
    int doc_get(const std::string& doc_id, CortrixDoc& doc) override {
        auto it = docs.find(doc_id);
        if (it == docs.end()) return -2;
        doc = it->second;
        return 0;
    }
    int doc_update_status(const std::string&, DocStatus, const std::string&) override { return 0; }
    int doc_delete(const std::string&) override { return 0; }
    int doc_list_by_status(DocStatus, std::vector<CortrixDoc>&) override { return 0; }
    int doc_find_by_source(const std::string&, const std::string&, CortrixDoc&) override { return -2; }
    int doc_find_by_hash(const std::string&, CortrixDoc&) override { return -2; }
    int doc_count(int64_t*) override { return 0; }
    int block_insert(CortrixBlock&) override { return 0; }
    int block_get(uint64_t block_id, CortrixBlock& block) override {
        auto it = blocks.find(block_id);
        if (it == blocks.end()) return -2;
        block = it->second;
        return 0;
    }
    int block_get_by_doc(const std::string& doc_id, std::vector<CortrixBlock>& out) override {
        for (const auto& [bid, blk] : blocks) {
            (void)bid;
            if (blk.doc_id == doc_id) out.push_back(blk);
        }
        std::sort(out.begin(), out.end(),
                  [](const CortrixBlock& a, const CortrixBlock& b) {
                      return a.chunk_index < b.chunk_index;
                  });
        return 0;
    }
    int block_delete_by_doc(const std::string&) override { return 0; }
    int block_count(int64_t*) override { return 0; }
    int search_fulltext(const std::string&, int, std::vector<SearchResult>&) override { return 0; }
    int search_metadata(const std::string&, int, std::vector<SearchResult>&) override { return -1; }
};

class QueryPipelineArmsDocId : public ::testing::Test {
protected:
    void SetUp() override {
        embedder_ = std::make_unique<OnnxEmbedder>("", 128);
        embedder_->Init();

        // Two ordered chunks for the target doc.
        CortrixDoc doc;
        doc.doc_id = "arms-doc-1";
        doc.source_path = "/arms/doc1.pdf";
        doc.created_at = "2025-01-01T00:00:00Z";
        store_.docs["arms-doc-1"] = doc;

        CortrixBlock b0{301, "arms-doc-1", /*chunk_index=*/0, 1, 2, -1, "", {}, "chunk zero"};
        CortrixBlock b1{302, "arms-doc-1", /*chunk_index=*/1, 1, 2, -1, "", {}, "chunk one"};
        store_.blocks[301] = b0;
        store_.blocks[302] = b1;
    }

    QueryPipeline MakePipeline() {
        return QueryPipeline(*vec_searcher_, *bm25_searcher_, *fusion_,
                             *classifier_, *post_filter_, *deg_mgr_, *sql_stub_);
    }

    void BuildDeps() {
        LlmConfig llm_config;  // empty -> keyword mode
        classifier_ = std::make_unique<IntentClassifier>(llm_config);
        fusion_ = std::make_unique<RRFFusion>(60);
        vec_searcher_ = std::make_unique<VectorSearcher>(fake_vec_, *embedder_);
        bm25_searcher_ = std::make_unique<BM25Searcher>(store_);
        post_filter_ = std::make_unique<PostFilter>(store_);
        deg_mgr_ = std::make_unique<DegradationManager>();
        sql_stub_ = std::make_unique<SqlExecutorStub>();
    }

    std::unique_ptr<OnnxEmbedder> embedder_;
    cortrix::test::FakeIndex fake_vec_;
    DocIdFakeStore store_;

    std::unique_ptr<IntentClassifier> classifier_;
    std::unique_ptr<RRFFusion> fusion_;
    std::unique_ptr<VectorSearcher> vec_searcher_;
    std::unique_ptr<BM25Searcher> bm25_searcher_;
    std::unique_ptr<PostFilter> post_filter_;
    std::unique_ptr<DegradationManager> deg_mgr_;
    std::unique_ptr<SqlExecutorStub> sql_stub_;
};

TEST_F(QueryPipelineArmsDocId, Execute_DocIdFastPath_BypassesRoutes) {
    BuildDeps();
    QueryPipeline pipeline = MakePipeline();

    QueryRequest req;
    req.query = "ignored on the doc_id fast path";
    req.namespace_name = "default";
    req.top_k = 10;
    req.timeout_ms = 5000;
    req.filters.doc_id = "arms-doc-1";  // <-- engages the fast path (lines 34-49)

    QueryResponse response = pipeline.Execute(req);

    // The fast path returns the doc's chunks in order and labels the route "doc_id".
    EXPECT_FALSE(response.has_error);
    EXPECT_EQ(response.results.size(), 2u);
    EXPECT_EQ(response.meta.total_results, 2);
    EXPECT_FALSE(response.meta.degraded);
    EXPECT_EQ(response.meta.degradation_level, 0);
    ASSERT_EQ(response.meta.routes_used.size(), 1u);
    EXPECT_EQ(response.meta.routes_used[0], "doc_id");
    EXPECT_GE(response.meta.latency_ms, 0);
    // chunk_index order preserved.
    EXPECT_EQ(response.results[0].block_id, 301);
    EXPECT_EQ(response.results[1].block_id, 302);
}

TEST_F(QueryPipelineArmsDocId, Execute_DocIdFastPath_UnknownDocReturnsEmpty) {
    BuildDeps();
    QueryPipeline pipeline = MakePipeline();

    QueryRequest req;
    req.query = "anything";
    req.namespace_name = "default";
    req.top_k = 5;
    req.timeout_ms = 5000;
    req.filters.doc_id = "no-such-doc";  // still takes the fast path; just yields 0 rows

    QueryResponse response = pipeline.Execute(req);

    EXPECT_FALSE(response.has_error);
    EXPECT_EQ(response.results.size(), 0u);
    EXPECT_EQ(response.meta.total_results, 0);
    ASSERT_EQ(response.meta.routes_used.size(), 1u);
    EXPECT_EQ(response.meta.routes_used[0], "doc_id");
}

}  // namespace
}  // namespace cortrix

// ===========================================================================
// OnnxComplexityArms — Init() arms reachable WITHOUT real inference.
//
// These need the LOCAL model file present + ONNX Runtime linked (offline build
// skips). They are deterministic (local file I/O, no network):
//
//   - VocabLoadFailureSurfacedFromInit  : model present but a BAD vocab path →
//       tokenizer_.LoadVocab() fails → Init() warns + returns the error and stays
//       unavailable (onnx_complexity_backend.cpp 66-71, esp. the 68-70 warn/return).
//   - AutoThreadCountInitSucceeds        : intra_threads=0 → the auto thread-count
//       arm (81-82) runs during a successful Init().
//   - InferSuccessMapsArgmax             : a real Infer() over the loaded session
//       exercises the encode + Run + argmax/confidence success path (124-200).
//
// Lines that require a *transient ONNX fault* (104-109 session-init catch, 131-132
// tokenization-fail, 168-171/174 Run-retry/throw) or a malformed probs tensor
// (183-184) are NOT deterministically triggerable from a valid local model without
// fault injection into src/; documented, not covered here.
// ===========================================================================
namespace cortrix::query {
namespace {

const std::string kModelDir =
    std::string(CORTRIX_CE_SOURCE_DIR) + "/models/query-complexity";

std::string ModelPath() { return kModelDir + "/model.onnx"; }
std::string VocabPath() { return kModelDir + "/vocab.txt"; }

bool ModelPresent() { return fs::exists(ModelPath()); }

// Init() with a present model but a vocab.txt that lacks the required special
// tokens → LoadVocab returns Internal → Init() warns + returns that error and
// leaves the backend unavailable (cpp 66-71). Covers the 68-70 warn/return arm,
// which the offline "missing model" tests never reach (they return before
// LoadVocab under the model-absent guard).
TEST(OnnxComplexityArms, VocabLoadFailureSurfacedFromInit) {
    if (!ModelPresent()) {
        GTEST_SKIP() << "query-complexity model.onnx absent (offline build)";
    }

    // A vocab missing [CLS]/[SEP]/[UNK]/[PAD] → LoadVocab() fails with Internal.
    fs::path bad = fs::temp_directory_path() / "cortrix_arms_bad_vocab.txt";
    {
        std::ofstream out(bad);
        out << "the\nquick\nbrown\nfox\n";  // no required special tokens
    }

    OnnxComplexityBackend backend(ModelPath(), bad.string());
    Status st = backend.Init();
    EXPECT_FALSE(st.ok());                 // surfaced from LoadVocab
    EXPECT_FALSE(backend.IsAvailable());   // stays unavailable on load failure

    std::error_code ec;
    fs::remove(bad, ec);
}

// intra_threads = 0 → Init() takes the auto thread-count arm (cpp 81-82) before
// creating the session. With the real model + ONNX linked the backend becomes
// available; if ONNX is present but unlinked in this build, Init() may still report
// unavailable — we only assert that Init() does not error in the loadable case.
TEST(OnnxComplexityArms, AutoThreadCountInitSucceeds) {
    if (!ModelPresent()) {
        GTEST_SKIP() << "query-complexity model.onnx absent (offline build)";
    }
    OnnxComplexityBackend backend(ModelPath(), VocabPath(),
                                  /*max_seq_length=*/64, /*intra_threads=*/0);
    Status st = backend.Init();
    if (!backend.IsAvailable()) {
        GTEST_SKIP() << "ONNX Runtime not linked / model unloadable: " << st.message();
    }
    EXPECT_TRUE(st.ok());
    EXPECT_TRUE(backend.IsAvailable());
    // Idempotent second Init() stays ready.
    EXPECT_TRUE(backend.Init().ok());
    EXPECT_TRUE(backend.IsAvailable());
}

// A real Infer() over the loaded session exercises the tokenize → Run → argmax →
// {label,confidence} success path (cpp 124-200). The label must be one of the three
// known classes and the confidence a valid softmax value.
TEST(OnnxComplexityArms, InferSuccessMapsArgmax) {
    if (!ModelPresent()) {
        GTEST_SKIP() << "query-complexity model.onnx absent (offline build)";
    }
    OnnxComplexityBackend backend(ModelPath(), VocabPath());
    Status st = backend.Init();
    if (!backend.IsAvailable()) {
        GTEST_SKIP() << "ONNX Runtime not linked / model unloadable: " << st.message();
    }

    ComplexityBackendResult r = backend.Infer("Summarize the key risks in this contract.");
    EXPECT_TRUE(r.label == "chat" || r.label == "simple" || r.label == "complex")
        << "unexpected label: " << r.label;
    // softmax confidence is a float in (0, 1].
    EXPECT_GT(r.confidence, 0.0f);
    EXPECT_LE(r.confidence, 1.0001f);
}

}  // namespace
}  // namespace cortrix::query
