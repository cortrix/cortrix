#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <string>

namespace cortrix::store {

/// The parent_chunk_store subsystem metrics (observability
/// subsystem `parent_chunk_store`). Mirrors the AgentTraceMetrics /
/// SparseMetrics template (process-wide singleton, atomic counters + histogram,
/// OpenMetrics renderer).
///
/// §2.5 metric schema (V1.0 — the third row, cache_hit_ratio, is deferred to the
/// D3 LRU cache layer and is NOT emitted here):
///   cortrix_parent_chunk_store_lookup_total           counter   {result=hit|miss}
///   cortrix_parent_chunk_store_lookup_latency_seconds histogram (no label)
///
/// Cardinality control (OBS_SPEC §3.2): the only label is the enum `result`
/// (hit/miss) — NO parent_id / ns_id / doc_id labels (high-cardinality, forbidden;
/// those go to structured logs).
///
/// D3 standalone: a self-contained, dependency-free recorder + an OpenMetrics
/// text renderer. The `/metrics` scrape endpoint does not exist in the frozen
/// tree — registering this recorder into that endpoint is cross-Feature wiring
/// deferred to D3.5. Until then it is fully usable + testable in-process and
/// RenderOpenMetrics() produces what the server will serve.
class ParentChunkStoreMetrics {
public:
    /// result label for cortrix_parent_chunk_store_lookup_total (§2.5).
    enum class LookupResult { kHit = 0, kMiss };

    /// Process-wide instance (metrics are global counters/histogram).
    static ParentChunkStoreMetrics& Instance();

    /// Public default ctor: enables isolated per-test instances and the
    /// SqliteParentChunkStore metrics-injection seam. Production code uses the
    /// process-wide Instance(); the store defaults to it when none is injected.
    ParentChunkStoreMetrics() = default;

    // --- cortrix_parent_chunk_store_lookup_total (Counter, label: result) ---
    void RecordLookup(LookupResult result);
    uint64_t LookupCount(LookupResult result) const;

    // --- cortrix_parent_chunk_store_lookup_latency_seconds (Histogram, no label) ---
    // Observes one lookup's wall-clock latency in seconds.
    void ObserveLookupLatency(double seconds);
    uint64_t LookupLatencyCount() const;
    double LookupLatencySum() const;

    /// Render all recorded metrics as OpenMetrics/Prometheus text exposition.
    std::string RenderOpenMetrics() const;

    /// Reset all counters/histogram (test-only — production metrics are monotonic).
    void ResetForTest();

    /// Number of explicit `le` buckets in the lookup_latency_seconds histogram
    /// (a trailing +Inf bucket is implicit). Parent lookups are a single indexed
    /// SQLite point query, so the bounds are sub-second.
    static constexpr int kNumDurBuckets = 8;  // {0.0001,0.00025,0.0005,0.001,0.0025,0.005,0.01,0.05}

private:
    static constexpr int kLookupResultCount = 2;  // hit / miss

    std::array<std::atomic<uint64_t>, kLookupResultCount> lookup_{};

    // lookup_latency_seconds histogram (single series, no label). Sum kept in
    // micros for atomic accumulation; rendered as seconds.
    std::atomic<uint64_t> lat_sum_us_{0};
    std::atomic<uint64_t> lat_count_{0};
    std::array<std::atomic<uint64_t>, kNumDurBuckets + 1> lat_bkt_{};  // last = +Inf
};

const char* ToString(ParentChunkStoreMetrics::LookupResult result);

}  // namespace cortrix::store
