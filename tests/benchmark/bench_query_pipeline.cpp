#include <benchmark/benchmark.h>
#include <filesystem>
#include <random>
#include <string>
#include "cortrix/store/cortrix_store_sqlite.h"
#include "cortrix/store/cortrix_vector_hnswlib.h"
#include "cortrix/spc/onnx_embedder.h"
#include "cortrix/query/vector_searcher.h"
#include "cortrix/query/bm25_searcher.h"
#include "cortrix/query/rrf_fusion.h"
#include "cortrix/query/intent_classifier.h"
#include "cortrix/query/post_filter.h"
#include "cortrix/query/degradation_manager.h"
#include "cortrix/query/sql_executor.h"
#include "cortrix/query/query_pipeline.h"
#include "cortrix/query/query_request.h"

namespace fs = std::filesystem;

static constexpr int kDim = 128;

// ---------------------------------------------------------------------------
// Shared fixture: store + vector index + pipeline components
// ---------------------------------------------------------------------------

struct PipelineEnv {
    fs::path dir;
    std::unique_ptr<cortrix::CortrixStoreSqlite> store;
    std::unique_ptr<cortrix::CortrixVectorHnswlib> vec_index;
    std::unique_ptr<cortrix::OnnxEmbedder> embedder;
    std::unique_ptr<cortrix::VectorSearcher> vec_searcher;
    std::unique_ptr<cortrix::BM25Searcher> bm25_searcher;
    std::unique_ptr<cortrix::RRFFusion> fusion;
    std::unique_ptr<cortrix::IntentClassifier> classifier;
    std::unique_ptr<cortrix::PostFilter> post_filter;
    std::unique_ptr<cortrix::DegradationManager> degradation;
    std::unique_ptr<cortrix::SqlExecutorStub> sql_exec;
    std::unique_ptr<cortrix::QueryPipeline> pipeline;

    PipelineEnv(int scale, const std::string& label) {
        dir = fs::temp_directory_path() / ("bench_pipeline_" + label);
        fs::create_directories(dir);

        // Store
        store = std::make_unique<cortrix::CortrixStoreSqlite>(
            (dir / "bench.db").string());
        store->Open();

        // Vector index
        cortrix::HnswConfig hcfg;
        hcfg.dim = kDim;
        hcfg.max_elements = scale + 1000;
        hcfg.M = 16;
        hcfg.ef_construction = 200;
        hcfg.ef_search = 100;
        vec_index = std::make_unique<cortrix::CortrixVectorHnswlib>(
            (dir / "bench.idx").string(), hcfg);
        vec_index->Init();

        // Embedder (stub mode: dim=128, non-existent model returns random vecs)
        embedder = std::make_unique<cortrix::OnnxEmbedder>("", kDim);
        // Note: Init() would fail for stub, but VectorSearcher.Search calls
        // embedder.Embed which will fail. For benchmarks we pre-populate
        // the vector index so VectorSearcher still exercises the search path.

        // Seed data
        std::mt19937 rng(42);
        std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
        for (int i = 0; i < scale; ++i) {
            cortrix::CortrixDoc doc;
            doc.source_type = "bench";
            doc.source_path = "file_" + std::to_string(i) + ".txt";
            store->doc_create(doc);

            cortrix::CortrixBlock blk;
            blk.doc_id = doc.doc_id;
            blk.chunk_index = 0;
            blk.block_type = 1;
            blk.processing_level = 3;
            blk.content_text = "benchmark content for query pipeline test " +
                               std::to_string(i);
            blk.data = {0x00};  // minimal non-null blob
            store->block_insert(blk);

            std::vector<float> vec(kDim);
            for (auto& v : vec) v = dist(rng);
            vec_index->add(blk.block_id, vec.data(), kDim);
        }

        // Pipeline components
        vec_searcher = std::make_unique<cortrix::VectorSearcher>(
            *vec_index, *embedder);
        bm25_searcher = std::make_unique<cortrix::BM25Searcher>(*store);
        fusion = std::make_unique<cortrix::RRFFusion>(60);

        cortrix::LlmConfig llm_cfg;  // empty = keyword mode
        classifier = std::make_unique<cortrix::IntentClassifier>(llm_cfg);
        post_filter = std::make_unique<cortrix::PostFilter>(*store);
        degradation = std::make_unique<cortrix::DegradationManager>();
        sql_exec = std::make_unique<cortrix::SqlExecutorStub>();

        pipeline = std::make_unique<cortrix::QueryPipeline>(
            *vec_searcher, *bm25_searcher, *fusion,
            *classifier, *post_filter, *degradation, *sql_exec);
    }

    ~PipelineEnv() {
        store->Close();
        fs::remove_all(dir);
    }
};

// ---------------------------------------------------------------------------
// BM_QueryPipelineBM25Only – BM25-only query
// ---------------------------------------------------------------------------

static void BM_QueryPipelineBM25Only(benchmark::State& state) {
    PipelineEnv env(1000, "bm25_" + std::to_string(state.thread_index()));

    cortrix::QueryRequest req;
    req.query = "benchmark content pipeline";
    req.namespace_name = "default";
    req.top_k = 10;
    req.timeout_ms = 5000;
    req.search_config.enable_vector = false;
    req.search_config.enable_bm25 = true;
    req.search_config.enable_sql = false;

    for (auto _ : state) {
        auto resp = env.pipeline->Execute(req);
        benchmark::DoNotOptimize(resp);
    }
}
BENCHMARK(BM_QueryPipelineBM25Only);

// ---------------------------------------------------------------------------
// BM_QueryPipelineHybrid – hybrid mode (vector + BM25)
// Note: vector search will degrade since embedder is stub, benchmarks
//       the full pipeline path including degradation handling.
// ---------------------------------------------------------------------------

static void BM_QueryPipelineHybrid(benchmark::State& state) {
    PipelineEnv env(1000, "hybrid_" + std::to_string(state.thread_index()));

    cortrix::QueryRequest req;
    req.query = "benchmark content pipeline";
    req.namespace_name = "default";
    req.top_k = 10;
    req.timeout_ms = 5000;
    req.search_config.enable_vector = true;
    req.search_config.enable_bm25 = true;
    req.search_config.enable_sql = false;

    for (auto _ : state) {
        auto resp = env.pipeline->Execute(req);
        benchmark::DoNotOptimize(resp);
    }
}
BENCHMARK(BM_QueryPipelineHybrid);

// ---------------------------------------------------------------------------
// BM_IntentClassification – keyword-mode intent classification overhead
// ---------------------------------------------------------------------------

static void BM_IntentClassification(benchmark::State& state) {
    cortrix::LlmConfig llm_cfg;  // keyword mode
    cortrix::IntentClassifier classifier(llm_cfg);

    for (auto _ : state) {
        auto intent = classifier.Classify("SELECT * FROM users WHERE id = 5",
                                          5000000);
        benchmark::DoNotOptimize(intent);
    }
}
BENCHMARK(BM_IntentClassification);

// ---------------------------------------------------------------------------
// BM_RRFFusion – RRF merge at various route counts
// ---------------------------------------------------------------------------

static void BM_RRFFusion(benchmark::State& state) {
    const int items = static_cast<int>(state.range(0));
    cortrix::RRFFusion fusion(60);

    // Build two route results
    cortrix::RouteResult vec_route;
    vec_route.route_name = "vector";
    vec_route.status = cortrix::RouteStatus::kSuccess;
    for (int i = 0; i < items; ++i) {
        cortrix::RouteResultItem item;
        item.block_id = i;
        item.doc_id = i;
        item.raw_score = 1.0f / (1.0f + static_cast<float>(i));
        vec_route.items.push_back(item);
    }

    cortrix::RouteResult bm25_route;
    bm25_route.route_name = "bm25";
    bm25_route.status = cortrix::RouteStatus::kSuccess;
    for (int i = 0; i < items; ++i) {
        cortrix::RouteResultItem item;
        item.block_id = i + items / 2;  // partial overlap
        item.doc_id = i;
        item.raw_score = 1.0f / (1.0f + static_cast<float>(i));
        bm25_route.items.push_back(item);
    }

    std::vector<cortrix::RouteResult> routes = {vec_route, bm25_route};

    for (auto _ : state) {
        auto merged = fusion.Merge(routes);
        benchmark::DoNotOptimize(merged);
    }
}
BENCHMARK(BM_RRFFusion)->Arg(30)->Arg(100)->Arg(300);
