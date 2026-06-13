#include <benchmark/benchmark.h>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>
#include "cortrix/store/cortrix_store_sqlite.h"
#include "cortrix/query/bm25_searcher.h"

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static fs::path MakeTempDir(const std::string& label) {
    auto dir = fs::temp_directory_path() / ("bench_bm25_" + label);
    fs::create_directories(dir);
    return dir;
}

// Vocabulary pool for varied content
static const char* kWords[] = {
    "cortrix", "semantic", "search", "vector", "database", "document",
    "memory", "pipeline", "block", "chunk", "embed", "index",
    "query", "fusion", "filter", "namespace", "storage", "benchmark",
    "performance", "latency", "throughput", "engine", "server", "api",
    "content", "metadata", "schema", "table", "column", "result"
};
static constexpr int kWordCount = sizeof(kWords) / sizeof(kWords[0]);

static std::string MakeContent(int i) {
    std::string s;
    for (int w = 0; w < 20; ++w) {
        if (w > 0) s += ' ';
        s += kWords[(i * 7 + w * 3) % kWordCount];
    }
    s += " item_" + std::to_string(i);
    return s;
}

static std::unique_ptr<cortrix::CortrixStoreSqlite> SeedStore(const fs::path& dir, int n) {
    auto store = std::make_unique<cortrix::CortrixStoreSqlite>((dir / "bench.db").string());
    store->Open();

    for (int i = 0; i < n; ++i) {
        cortrix::CortrixDoc doc;
        doc.source_type = "bench";
        doc.source_path = "file_" + std::to_string(i) + ".txt";
        store->doc_create(doc);

        cortrix::CortrixBlock blk;
        blk.doc_id = doc.doc_id;
        blk.chunk_index = 0;
        blk.block_type = 1;
        blk.processing_level = 3;
        blk.content_text = MakeContent(i);
        blk.data = {0x00};  // minimal non-null blob
        store->block_insert(blk);
    }
    return store;
}

// ---------------------------------------------------------------------------
// BM_BM25SearchDirect – raw FTS5 search via CortrixStore
// ---------------------------------------------------------------------------

static void BM_BM25SearchDirect(benchmark::State& state) {
    const int scale = static_cast<int>(state.range(0));
    auto dir = MakeTempDir("direct_" + std::to_string(scale));
    auto store = SeedStore(dir, scale);

    for (auto _ : state) {
        std::vector<cortrix::SearchResult> results;
        benchmark::DoNotOptimize(
            store->search_fulltext("semantic search engine", 10, results));
    }

    store->Close();
    fs::remove_all(dir);
}
BENCHMARK(BM_BM25SearchDirect)->Arg(100)->Arg(1000)->Arg(10000);

// ---------------------------------------------------------------------------
// BM_BM25Searcher – via BM25Searcher class
// ---------------------------------------------------------------------------

static void BM_BM25Searcher(benchmark::State& state) {
    const int scale = static_cast<int>(state.range(0));
    auto dir = MakeTempDir("searcher_" + std::to_string(scale));
    auto store = SeedStore(dir, scale);

    cortrix::BM25Searcher searcher(*store);
    int64_t timeout_us = 5000000;  // 5 seconds

    for (auto _ : state) {
        auto result = searcher.Search("cortrix database", 10, timeout_us);
        benchmark::DoNotOptimize(result);
    }

    store->Close();
    fs::remove_all(dir);
}
BENCHMARK(BM_BM25Searcher)->Arg(100)->Arg(1000)->Arg(10000);

// ---------------------------------------------------------------------------
// BM_BM25SearchTopK – vary top_k at fixed 10K scale
// ---------------------------------------------------------------------------

static void BM_BM25SearchTopK(benchmark::State& state) {
    const int top_k = static_cast<int>(state.range(0));
    auto dir = MakeTempDir("topk_" + std::to_string(top_k));
    auto store = SeedStore(dir, 10000);

    for (auto _ : state) {
        std::vector<cortrix::SearchResult> results;
        benchmark::DoNotOptimize(
            store->search_fulltext("vector pipeline", top_k, results));
    }

    store->Close();
    fs::remove_all(dir);
}
BENCHMARK(BM_BM25SearchTopK)->Arg(1)->Arg(10)->Arg(50)->Arg(100);

// ---------------------------------------------------------------------------
// BM_BM25SearchLongQuery – longer query strings
// ---------------------------------------------------------------------------

static void BM_BM25SearchLongQuery(benchmark::State& state) {
    auto dir = MakeTempDir("longq");
    auto store = SeedStore(dir, 10000);

    std::string long_query =
        "cortrix semantic search vector database document memory pipeline "
        "block chunk embed index query fusion filter namespace storage";

    for (auto _ : state) {
        std::vector<cortrix::SearchResult> results;
        benchmark::DoNotOptimize(
            store->search_fulltext(long_query, 10, results));
    }

    store->Close();
    fs::remove_all(dir);
}
BENCHMARK(BM_BM25SearchLongQuery);
