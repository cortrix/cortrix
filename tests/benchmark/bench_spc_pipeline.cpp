#include <benchmark/benchmark.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include "cortrix/spc/recursive_chunker.h"
#include "cortrix/spc/block_assembler.h"
#include "cortrix/spc/onnx_embedder.h"
#include "cortrix/store/cortrix_store_sqlite.h"
// [P6] cortrix_vector_hnswlib.h removed: the MVP vector store was deleted (P-HNSW
// replaced it) and this benchmark never used the class — only the stale include
// remained.

namespace fs = std::filesystem;

static constexpr int kDim = 128;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static fs::path MakeTempDir(const std::string& label) {
    auto dir = fs::temp_directory_path() / ("bench_spc_" + label);
    fs::create_directories(dir);
    return dir;
}

// Generate a sample text file with N paragraphs
static std::string GenerateText(int paragraphs) {
    std::string text;
    for (int i = 0; i < paragraphs; ++i) {
        text += "This is paragraph " + std::to_string(i) +
                " of the benchmark document. It contains enough words to test "
                "the recursive chunking algorithm with various separator types. "
                "The content includes technical terms like cortrix, semantic search, "
                "vector database, and document processing pipeline.\n\n";
    }
    return text;
}

static void WriteTextFile(const fs::path& path, const std::string& content) {
    std::ofstream f(path);
    f << content;
}

// Read a text file into a string. In-process equivalent of the (removed in R6b)
// MVP TxtParser used purely to obtain text for the chunker; the F06 parser path
// runs via a Python subprocess bridge and is not appropriate for a benchmark.
static std::string ReadTextFile(const fs::path& path) {
    std::ifstream f(path, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// ---------------------------------------------------------------------------
// BM_RecursiveChunker – chunking throughput at various sizes
// ---------------------------------------------------------------------------

static void BM_RecursiveChunker(benchmark::State& state) {
    const int paragraphs = static_cast<int>(state.range(0));
    std::string text = GenerateText(paragraphs);

    cortrix::ChunkConfig cfg;
    cfg.chunk_size = 512;
    cfg.chunk_overlap = 50;
    cortrix::RecursiveChunker chunker(cfg);

    for (auto _ : state) {
        auto chunks = chunker.Chunk(text);
        benchmark::DoNotOptimize(chunks);
    }

    state.SetBytesProcessed(state.iterations() *
                            static_cast<int64_t>(text.size()));
}
BENCHMARK(BM_RecursiveChunker)->Arg(10)->Arg(50)->Arg(200);

// NOTE: BM_TxtParser / BM_MarkdownParser were removed in R6b. They benchmarked
// the deleted MVP parser path (cortrix::TxtParser / cortrix::MarkdownParser),
// which has no F06 equivalent for plain text/markdown (F06 DoclingParser /
// PaddleOCRParser run via a Python subprocess bridge, not in-process). The
// chunker / pipeline benchmarks below remain the meaningful SPC throughput tests.

// ---------------------------------------------------------------------------
// BM_BlockAssembler – block assembly with mock embeddings
// ---------------------------------------------------------------------------

static void BM_BlockAssembler(benchmark::State& state) {
    cortrix::BlockAssembler assembler;

    cortrix::ChunkResult chunk;
    chunk.text = "Test chunk content for benchmark assembly with enough "
                 "words to simulate real data processing.";
    chunk.chunk_index = 0;
    chunk.start_offset = 0;
    chunk.end_offset = static_cast<int>(chunk.text.size());
    chunk.token_count_approx = 15;

    cortrix::EmbeddingResult embedding;
    embedding.vector.resize(kDim, 0.5f);
    embedding.dim = kDim;

    for (auto _ : state) {
        auto block = assembler.Assemble("1", chunk, embedding,  // doc_id is a string
                                        cortrix::kBlockFile);
        benchmark::DoNotOptimize(block);
    }
}
BENCHMARK(BM_BlockAssembler);

// ---------------------------------------------------------------------------
// BM_SPCFullPath – parse + chunk + assemble (no embedding, no store write)
// ---------------------------------------------------------------------------

static void BM_SPCFullPath(benchmark::State& state) {
    auto dir = MakeTempDir("full_path");
    auto file = dir / "test.txt";
    WriteTextFile(file, GenerateText(50));

    cortrix::ChunkConfig chunk_cfg;
    chunk_cfg.chunk_size = 512;
    chunk_cfg.chunk_overlap = 50;
    cortrix::RecursiveChunker chunker(chunk_cfg);
    cortrix::BlockAssembler assembler;

    // Mock embedding result
    cortrix::EmbeddingResult mock_emb;
    mock_emb.vector.resize(kDim, 0.1f);
    mock_emb.dim = kDim;

    for (auto _ : state) {
        // Read text (replaces MVP parser; see ReadTextFile note)
        std::string text = ReadTextFile(file.string());

        // Chunk
        auto chunks = chunker.Chunk(text);

        // Assemble
        for (size_t i = 0; i < chunks.size(); ++i) {
            auto block = assembler.Assemble("1", chunks[i], mock_emb,  // doc_id string
                                            cortrix::kBlockFile);
            benchmark::DoNotOptimize(block);
        }
    }

    state.counters["docs_per_sec"] = benchmark::Counter(
        static_cast<double>(state.iterations()),
        benchmark::Counter::kIsRate);

    fs::remove_all(dir);
}
BENCHMARK(BM_SPCFullPath);
