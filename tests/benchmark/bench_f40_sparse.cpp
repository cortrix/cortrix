#include <benchmark/benchmark.h>

#include <map>
#include <string>
#include <vector>

#include "cortrix/retrieval/sparse_codec.h"
#include "cortrix/retrieval/sparse_rrf.h"
#include "cortrix/retrieval/splade_sparse_retriever.h"

// F40 S11 benchmarks (detailed-design §13.bis 2.1/2.2). Synthetic-data micro-
// benchmarks for the inverted-index write/query + sparse_vec codec. These
// validate the *algorithm* cost shape standalone; the full §13.bis perf gates
// (N=1M write <2h, query N=1M P50<100ms) + the §2.3 BEIR Recall@10 hybrid vs
// dense-only +3pp hard threshold need the real BGE-M3 model + BEIR fiqa dataset
// → run at D3.5 (real model/dataset not present in the standalone tree).
namespace {

using namespace cortrix::retrieval;

SparseVector MakeVec(int seed, int num_terms) {
    SparseVector v;
    for (int i = 0; i < num_terms; ++i) {
        uint32_t term = static_cast<uint32_t>((seed * 131 + i * 17) % 65535) + 1;
        v.terms[term] = 0.01f * static_cast<float>((i % 100) + 1);
    }
    return v;
}

// --- codec: serialize / deserialize a K=100 sparse_vec ---
static void BM_SparseCodecRoundTrip(benchmark::State& state) {
    SparseVector v = MakeVec(42, static_cast<int>(state.range(0)));
    for (auto _ : state) {
        auto blob = SerializeSparseVec(v);
        auto back = DeserializeSparseVec(blob);
        benchmark::DoNotOptimize(back);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_SparseCodecRoundTrip)->Arg(50)->Arg(100)->Arg(200);

// --- inverted index: Add N chunks (write path, §6.1) ---
static void BM_SpladeAdd(benchmark::State& state) {
    const int n = static_cast<int>(state.range(0));
    for (auto _ : state) {
        state.PauseTiming();
        SpladeSparseRetriever idx(SpladeConfig{}, ":memory:");
        idx.Open();
        state.ResumeTiming();
        for (int i = 0; i < n; ++i) {
            idx.Add("ns", "c" + std::to_string(i), MakeVec(i, 100));
        }
    }
    state.SetItemsProcessed(state.iterations() * n);
}
BENCHMARK(BM_SpladeAdd)->Arg(1000)->Arg(5000);

// --- inverted index: Search over a seeded index (query path, §6.3) ---
static void BM_SpladeSearch(benchmark::State& state) {
    const int n = static_cast<int>(state.range(0));
    SpladeSparseRetriever idx(SpladeConfig{}, ":memory:");
    idx.Open();
    for (int i = 0; i < n; ++i) {
        idx.Add("ns", "c" + std::to_string(i), MakeVec(i, 100));
    }
    SparseVector q = MakeVec(7, 30);  // ~30-term query (§4.3 query-term estimate)
    for (auto _ : state) {
        auto hits = idx.Search(q, "ns", 100);
        benchmark::DoNotOptimize(hits);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_SpladeSearch)->Arg(1000)->Arg(10000);

// --- 5-path RRF fusion cost ---
static void BM_FivePathRrf(benchmark::State& state) {
    const int per_path = static_cast<int>(state.range(0));
    FivePathInput in;
    auto fill = [&](std::vector<SparseHit>& l, int base) {
        for (int i = 0; i < per_path; ++i) {
            l.push_back({"c" + std::to_string((base + i) % (per_path * 2)),
                         1.0f / (i + 1)});
        }
    };
    fill(in.dense, 0);
    fill(in.sparse, 1);
    fill(in.fts5, 2);
    for (auto _ : state) {
        auto out = FuseFivePathRrf(in, 100);
        benchmark::DoNotOptimize(out);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_FivePathRrf)->Arg(50)->Arg(200);

}  // namespace
