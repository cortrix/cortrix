#include <benchmark/benchmark.h>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>
#include "cortrix/store/cortrix_store_sqlite.h"

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static fs::path MakeTempDir(const std::string& label) {
    auto dir = fs::temp_directory_path() / ("bench_store_" + label);
    fs::create_directories(dir);
    return dir;
}

static std::unique_ptr<cortrix::CortrixStoreSqlite> OpenFreshStore(const fs::path& dir) {
    auto store = std::make_unique<cortrix::CortrixStoreSqlite>((dir / "bench.db").string());
    store->Open();
    return store;
}

// Seed N documents + 1 block each (for search benchmarks)
static void SeedBlocks(cortrix::CortrixStoreSqlite& store, int n) {
    for (int i = 0; i < n; ++i) {
        cortrix::CortrixDoc doc;
        doc.source_type = "bench";
        doc.source_path = "file_" + std::to_string(i) + ".txt";
        store.doc_create(doc);

        cortrix::CortrixBlock blk;
        blk.doc_id = doc.doc_id;
        blk.chunk_index = 0;
        blk.block_type = 1;
        blk.processing_level = 3;
        blk.content_text = "benchmark test content for document number " +
                           std::to_string(i) + " with some searchable words";
        blk.data = {0x00};  // minimal non-null blob
        store.block_insert(blk);
    }
}

// ---------------------------------------------------------------------------
// BM_DocCreate – single document creation
// ---------------------------------------------------------------------------

static void BM_DocCreate(benchmark::State& state) {
    auto dir = MakeTempDir("doc_create");
    auto store = OpenFreshStore(dir);

    for (auto _ : state) {
        cortrix::CortrixDoc doc;
        doc.source_type = "bench";
        doc.source_path = "test.txt";
        store->doc_create(doc);
    }

    store->Close();
    fs::remove_all(dir);
}
BENCHMARK(BM_DocCreate);

// ---------------------------------------------------------------------------
// BM_DocGet – single document retrieval
// ---------------------------------------------------------------------------

static void BM_DocGet(benchmark::State& state) {
    auto dir = MakeTempDir("doc_get");
    auto store = OpenFreshStore(dir);

    cortrix::CortrixDoc seed;
    seed.source_type = "bench";
    seed.source_path = "test.txt";
    store->doc_create(seed);
    std::string id = seed.doc_id;  // doc_id is a ULID string (Phase-1 migration)

    for (auto _ : state) {
        cortrix::CortrixDoc out;
        benchmark::DoNotOptimize(store->doc_get(id, out));
    }

    store->Close();
    fs::remove_all(dir);
}
BENCHMARK(BM_DocGet);

// ---------------------------------------------------------------------------
// BM_BlockInsert – single block insertion
// ---------------------------------------------------------------------------

static void BM_BlockInsert(benchmark::State& state) {
    auto dir = MakeTempDir("block_ins");
    auto store = OpenFreshStore(dir);

    cortrix::CortrixDoc doc;
    doc.source_type = "bench";
    doc.source_path = "test.txt";
    store->doc_create(doc);

    int idx = 0;
    for (auto _ : state) {
        cortrix::CortrixBlock blk;
        blk.doc_id = doc.doc_id;
        blk.chunk_index = idx++;
        blk.block_type = 1;
        blk.processing_level = 3;
        blk.content_text = "benchmark block content " + std::to_string(idx);
        blk.data = {0x00};  // minimal non-null blob
        store->block_insert(blk);
    }

    store->Close();
    fs::remove_all(dir);
}
BENCHMARK(BM_BlockInsert);

// ---------------------------------------------------------------------------
// BM_FTS5Search – full-text search at various scales
// ---------------------------------------------------------------------------

static void BM_FTS5Search(benchmark::State& state) {
    const int scale = static_cast<int>(state.range(0));
    auto dir = MakeTempDir("fts_" + std::to_string(scale));
    auto store = OpenFreshStore(dir);

    SeedBlocks(*store, scale);

    for (auto _ : state) {
        std::vector<cortrix::SearchResult> results;
        benchmark::DoNotOptimize(
            store->search_fulltext("benchmark searchable", 10, results));
    }

    store->Close();
    fs::remove_all(dir);
}
BENCHMARK(BM_FTS5Search)->Arg(100)->Arg(1000)->Arg(10000);

// ---------------------------------------------------------------------------
// BM_DocDelete – document deletion
// ---------------------------------------------------------------------------

static void BM_DocDelete(benchmark::State& state) {
    auto dir = MakeTempDir("doc_del");
    auto store = OpenFreshStore(dir);

    // Pre-create a pool of documents
    std::vector<std::string> ids;  // doc_id is a ULID string (Phase-1 migration)
    for (int i = 0; i < 10000; ++i) {
        cortrix::CortrixDoc doc;
        doc.source_type = "bench";
        doc.source_path = "del_" + std::to_string(i) + ".txt";
        store->doc_create(doc);
        ids.push_back(doc.doc_id);
    }

    size_t idx = 0;
    for (auto _ : state) {
        if (idx < ids.size()) {
            store->doc_delete(ids[idx++]);
        }
    }

    store->Close();
    fs::remove_all(dir);
}
BENCHMARK(BM_DocDelete);

// ---------------------------------------------------------------------------
// BM_DocCount – document count query
// ---------------------------------------------------------------------------

static void BM_DocCount(benchmark::State& state) {
    auto dir = MakeTempDir("doc_count");
    auto store = OpenFreshStore(dir);

    SeedBlocks(*store, 1000);

    for (auto _ : state) {
        int64_t count = 0;
        benchmark::DoNotOptimize(store->doc_count(&count));
    }

    store->Close();
    fs::remove_all(dir);
}
BENCHMARK(BM_DocCount);
