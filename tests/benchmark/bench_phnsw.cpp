// F01 S6 — P-HNSW benchmarks (design § 6 S6 / § 8 perf targets).
//
// Measures PHnsw directly (not the MVP CortrixVectorHnswlib): search latency
// (report P50/P99 via --benchmark_repetitions), single-write latency, batch
// write throughput, and crash-recovery time. Targets (design § 8):
//   search 1K blocks P50 <= 500us, P99 <= 1ms
//   single write <= 5ms ; batch write throughput >= 1000 vec/s
//   recovery of 10K WAL entries <= 2s
//
// Run: ./cortrix_benchmarks --benchmark_filter=PHnsw --benchmark_repetitions=5 \
//        --benchmark_report_aggregates_only=true

#include <benchmark/benchmark.h>

#include <unistd.h>

#include <filesystem>
#include <random>
#include <string>
#include <vector>

#include "cortrix/store/phnsw.h"
#include "cortrix/store/phnsw/wal_writer.h"
#include "cortrix/store/phnsw/wal_entry.h"

namespace fs = std::filesystem;

namespace {

constexpr int kDim = 1024;  // bge-m3 dimension (design baseline)

fs::path MakeTempDir(const std::string& label) {
    auto dir = fs::temp_directory_path() /
               ("bench_phnsw_" + label + "_" + std::to_string(::getpid()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

std::vector<float> RandomVec(std::mt19937& rng) {
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    std::vector<float> v(kDim);
    for (auto& x : v) x = dist(rng);
    return v;
}

cortrix::store::PhnswConfig BenchConfig(int max_elements) {
    cortrix::store::PhnswConfig c;
    c.dim = kDim;
    c.max_elements = max_elements;
    c.M = 16;
    c.ef_construction = 200;
    c.ef_search = 100;
    // Keep auto-snapshot out of the way during steady-state benchmarks.
    c.snapshot_max_wal_entries = 1'000'000;
    c.snapshot_max_wal_size_mb = 4096;
    return c;
}

}  // namespace

// --- Search latency (target: 1K blocks P50 <= 500us, P99 <= 1ms) ------------
static void BM_PHnswSearch(benchmark::State& state) {
    const int scale = static_cast<int>(state.range(0));
    auto dir = MakeTempDir("search_" + std::to_string(scale));
    cortrix::store::PHnsw index(dir.string(), BenchConfig(scale + 1000));

    std::mt19937 rng(42);
    for (int i = 0; i < scale; ++i) {
        auto v = RandomVec(rng);
        index.AddPoint(v.data(), static_cast<uint64_t>(i + 1));
    }
    std::vector<std::vector<float>> queries;
    for (int i = 0; i < 128; ++i) queries.push_back(RandomVec(rng));

    int qi = 0;
    for (auto _ : state) {
        auto r = index.Search(queries[qi % queries.size()].data(), 10);
        benchmark::DoNotOptimize(r);
        ++qi;
    }
    state.SetItemsProcessed(state.iterations());
    fs::remove_all(dir);
}
BENCHMARK(BM_PHnswSearch)->Arg(1000)->Arg(10000)->Unit(benchmark::kMicrosecond);

// --- Single-write latency (target: <= 5ms; durable WAL fsync per call) ------
static void BM_PHnswWriteSingle(benchmark::State& state) {
    auto dir = MakeTempDir("write1");
    cortrix::store::PHnsw index(dir.string(), BenchConfig(200000));

    std::mt19937 rng(42);
    uint64_t id = 1;
    for (auto _ : state) {
        state.PauseTiming();
        auto v = RandomVec(rng);
        state.ResumeTiming();
        index.AddPoint(v.data(), id++);
    }
    state.SetItemsProcessed(state.iterations());
    fs::remove_all(dir);
}
BENCHMARK(BM_PHnswWriteSingle)->Unit(benchmark::kMicrosecond);

// --- Batch write throughput (target: >= 1000 vec/s) -------------------------
static void BM_PHnswWriteBatch(benchmark::State& state) {
    const int batch = static_cast<int>(state.range(0));
    auto dir = MakeTempDir("writeb_" + std::to_string(batch));
    cortrix::store::PHnsw index(dir.string(), BenchConfig(2'000'000));

    std::mt19937 rng(42);
    // Pre-generate a pool of vectors so timing measures the write, not RNG.
    std::vector<std::vector<float>> pool;
    for (int i = 0; i < batch; ++i) pool.push_back(RandomVec(rng));

    uint64_t id = 1;
    for (auto _ : state) {
        std::vector<std::pair<const float*, uint64_t>> pts;
        pts.reserve(static_cast<size_t>(batch));
        for (int i = 0; i < batch; ++i) {
            pts.emplace_back(pool[static_cast<size_t>(i)].data(), id++);
        }
        index.AddPoints(pts);
    }
    // ItemsProcessed = total vectors written → benchmark reports items/s (vec/s).
    state.SetItemsProcessed(state.iterations() * batch);
    fs::remove_all(dir);
}
BENCHMARK(BM_PHnswWriteBatch)->Arg(100)->Arg(1000)->Unit(benchmark::kMillisecond);

// --- Recovery time (target: 10K WAL entries replay <= 2s) -------------------
// Builds a hnsw.wal with N records once, then times constructing a PHnsw (which
// runs recovery: load snapshot[none] + replay the whole WAL).
static void BM_PHnswRecovery(benchmark::State& state) {
    const int n = static_cast<int>(state.range(0));
    auto dir = MakeTempDir("recover_" + std::to_string(n));
    const std::string wal_path = (dir / "hnsw.wal").string();

    // Seed the WAL directly with N INSERT records (no snapshot → full replay).
    {
        std::mt19937 rng(42);
        auto wal = cortrix::store::WalWriter::Open(wal_path, kDim);
        std::vector<std::vector<uint8_t>> recs;
        recs.reserve(static_cast<size_t>(n));
        std::vector<std::vector<float>> vecs;
        vecs.reserve(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i) vecs.push_back(RandomVec(rng));
        for (int i = 0; i < n; ++i) {
            recs.push_back(cortrix::store::WalEntry::SerializeInsert(
                static_cast<uint64_t>(i + 1), vecs[static_cast<size_t>(i)].data(), kDim));
        }
        wal.value()->AppendBatch(recs);
    }

    for (auto _ : state) {
        cortrix::store::PHnsw index(dir.string(), BenchConfig(n + 1000));
        benchmark::DoNotOptimize(index.GetStats().vector_count);
    }
    state.SetItemsProcessed(state.iterations() * n);
    fs::remove_all(dir);
}
BENCHMARK(BM_PHnswRecovery)->Arg(10000)->Unit(benchmark::kMillisecond)->Iterations(3);
