#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <string>

#include "cortrix/agent_friendly/error.h"  // ErrorCategory (shared category label)

namespace cortrix::query {

/// The `scatter` subsystem metrics (observability subsystem
/// `scatter`). Naming `cortrix_scatter_<metric>_<unit>` (V2 ruling #3: cortrix_
/// prefix, no plugin prefix). Mirrors the RerankerMetrics template.
///
/// 🚨 standalone: a self-contained, dependency-free recorder + an OpenMetrics
/// text renderer. The `/metrics` scrape endpoint does not exist in the frozen
/// tree — registering this recorder into that endpoint is cross-Feature wiring
/// **deferred to integration**. Until then it is fully usable + testable in-process and
/// RenderOpenMetrics() produces what the server will serve.
///
/// metric schema (6 rows):
///   cortrix_scatter_requests_total          counter   {reason, category}
///   cortrix_scatter_failed_total            counter   {reason, category}
///   cortrix_scatter_namespaces_per_query    histogram (per-query NS count)
///   cortrix_scatter_dedup_collisions_total  counter   (cross-NS hash collisions —
///                                                      v1.0.2 rename, A-class
///                                                      "chunks_collapsed_count")
///   cortrix_scatter_partial_success_total   counter
///   cortrix_scatter_duration_seconds        histogram {namespace_count_bucket}
class ScatterMetrics {
public:
    /// reason label for requests_total / failed_total. `single` = 1-NS direct
    /// path (topic 1.6); `multi` = fan-out; `wildcard` = ["*"]-expanded request.
    enum class Reason {
        kSingle = 0,
        kMulti,
        kWildcard,
    };

    /// Process-wide instance (metrics are global counters/histograms).
    static ScatterMetrics& Instance();

    // --- cortrix_scatter_requests_total (Counter, labels: reason, category) ---
    // category here is the *request outcome* class (permanent on success, or the
    // failure category for a hard-rejected request); reason+category.
    void RecordRequest(Reason reason, agent_friendly::ErrorCategory category);
    uint64_t RequestCount(Reason reason, agent_friendly::ErrorCategory category) const;

    // --- cortrix_scatter_failed_total (Counter, labels: reason, category) ---
    void RecordFailed(Reason reason, agent_friendly::ErrorCategory category);
    uint64_t FailedCount(Reason reason, agent_friendly::ErrorCategory category) const;

    // --- cortrix_scatter_namespaces_per_query (Histogram) ---
    // Observes the per-query NS count; tracks per-`le` buckets + sum + count.
    // Bucket bounds {1,3,10,100} mirror the SLA NS tiers (count-type, so
    // integer bounds); `max_namespaces_per_query` defaults to 100 (range 1-1000).
    void ObserveNamespacesPerQuery(int n);
    uint64_t NamespacesPerQuerySum() const;
    uint64_t NamespacesPerQueryCount() const;
    /// Cumulative count at NS-bucket index (0:≤1, 1:≤3, 2:≤10, 3:≤100, 4:+Inf).
    uint64_t NamespacesPerQueryBucket(int bucket) const;

    // --- cortrix_scatter_dedup_collisions_total (Counter) ---
    // Incremented by the number of chunks collapsed in one cross-NS dedup pass
    // (A-class "chunks_collapsed_count" — v1.0.2 rename from dedup_applied_total).
    void RecordDedupCollisions(uint64_t collapsed);
    uint64_t DedupCollisionsCount() const;

    // --- cortrix_scatter_partial_success_total (Counter) ---
    // Incremented once per response with a non-empty meta.namespaces_failed[].
    void RecordPartialSuccess();
    uint64_t PartialSuccessCount() const;

    // --- cortrix_scatter_duration_seconds (Histogram, label: namespace_count_bucket) ---
    // Observes the overall scatter latency, bucketed by NS count (1 / 3 / 10 / 100
    // per the SLA tiers). Within each NS-count series tracks per-`le` latency
    // buckets {0.1,0.25,0.5,1,2,5,10,30}s (straddle the target/max tiers:
    // 0.5/0.6/1.0/5.0s targets, 1.5/1.8/3.0/15.0s max) + sum_ms + count.
    void ObserveDuration(int namespace_count, int latency_ms);

    /// The SLA bucket index for an NS count (0:≤1, 1:≤3, 2:≤10, 3:>10).
    static int DurationBucket(int namespace_count);
    uint64_t DurationSumMs(int bucket) const;
    uint64_t DurationCount(int bucket) const;
    /// Cumulative latency-bucket count within one NS-count series.
    /// `le_index` 0..kLatencyBuckets-1 = finite bounds, kLatencyBuckets = +Inf.
    uint64_t DurationLatencyBucket(int ns_bucket, int le_index) const;

    /// Render all recorded metrics as OpenMetrics/Prometheus text exposition.
    std::string RenderOpenMetrics() const;

    /// Reset all counters/histograms (test-only — production metrics are monotonic).
    void ResetForTest();

private:
    ScatterMetrics() = default;

    static constexpr int kReasonCount = 3;     // single / multi / wildcard
    static constexpr int kCategoryCount = 5;   // auth/quota/transient/permanent/timeout
    static constexpr int kDurationBuckets = 4; // ≤1 / ≤3 / ≤10 / >10
    // namespaces_per_query `le` bounds (count-type; SLA NS tiers).
    static constexpr int kNsBuckets = 4;       // ≤1 / ≤3 / ≤10 / ≤100 (+ +Inf)
    // duration_seconds `le` latency bounds (generic duration set straddling).
    static constexpr int kLatencyBuckets = 8;  // 0.1/0.25/0.5/1/2/5/10/30s (+ +Inf)

    static int Idx(Reason r, agent_friendly::ErrorCategory c);

    std::array<std::atomic<uint64_t>, kReasonCount * kCategoryCount> requests_{};
    std::array<std::atomic<uint64_t>, kReasonCount * kCategoryCount> failed_{};
    std::atomic<uint64_t> ns_per_query_sum_{0};
    std::atomic<uint64_t> ns_per_query_count_{0};
    // namespaces_per_query: one extra slot for the +Inf bucket.
    std::array<std::atomic<uint64_t>, kNsBuckets + 1> ns_per_query_bkt_{};
    std::atomic<uint64_t> dedup_collisions_{0};
    std::atomic<uint64_t> partial_success_{0};
    std::array<std::atomic<uint64_t>, kDurationBuckets> duration_sum_ms_{};
    std::array<std::atomic<uint64_t>, kDurationBuckets> duration_count_{};
    // Per NS-count series, the latency `le` buckets (one extra slot for +Inf).
    std::array<std::array<std::atomic<uint64_t>, kLatencyBuckets + 1>, kDurationBuckets>
        duration_lat_bkt_{};
};

const char* ToString(ScatterMetrics::Reason reason);

}  // namespace cortrix::query
