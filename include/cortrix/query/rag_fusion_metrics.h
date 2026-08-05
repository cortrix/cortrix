#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

#include "cortrix/query/rag_fusion_types.h"  // VariantStrategy (strategy label)

namespace cortrix::query {

/// The `rag_fusion` subsystem metrics (observability naming
/// `cortrix_rag_fusion_<metric>_<unit>`). Mirrors the ScatterMetrics template
/// (process-wide singleton, atomic counters/histograms, OpenMetrics renderer).
///
/// 🚨 Cardinality control: labels are
/// enum-only. NO `tenant_id` / `ns_id` / `user_id` (high-cardinality, forbidden);
/// per-NS / per-tenant data goes through the §3.4 per-tenant API. The model label
/// is a low-cardinality enum (< 50, provider × mainstream model — §8 Phase 2
/// re-review anchor) but represented as a free string here because models are
/// config-driven; the test `Metrics_RagFusionDegradedTotal_LabelEnum` (UT 21)
/// asserts no high-cardinality label string ever appears.
///
/// 🚨 D3 standalone: a self-contained, dependency-free recorder + an OpenMetrics
/// text renderer. The `/metrics` scrape endpoint does not exist in the frozen
/// tree — registering this recorder into that endpoint is cross-Feature wiring
/// **deferred to D3.5**. Until then it is fully usable + testable in-process and
/// RenderOpenMetrics() produces what the server will serve.
///
/// §8 metric schema (6 rows):
///   cortrix_rag_fusion_invocation_total          counter   {result}
///   cortrix_rag_fusion_variant_count             histogram {strategy}
///   cortrix_rag_fusion_llm_latency_seconds       histogram {model}
///   cortrix_rag_fusion_degraded_total            counter   {reason}
///   cortrix_rag_fusion_token_total               counter   {direction}
///   cortrix_rag_fusion_rrf_fusion_duration_seconds histogram (no labels)
class RagFusionMetrics {
public:
    /// result label for invocation_total (§8). `success` = variants generated;
    /// `degraded` = LLM failed → single-query fallback; `disabled` = NS disabled /
    /// skipped (no LLM call).
    enum class Result {
        kSuccess = 0,
        kDegraded,
        kDisabled,
    };

    /// reason label for degraded_total (§8) — the §7 degrade causes.
    enum class DegradeReason {
        kLlmTimeout = 0,
        kCircuitOpen,
        kQuotaExceeded,
        kInvalidResponse,
        kLlmTransport,
        kLlmHttp,
        kOther,
    };

    /// direction label for token_total (§8).
    enum class TokenDirection {
        kInput = 0,
        kOutput,
    };

    /// Process-wide instance (metrics are global counters/histograms).
    static RagFusionMetrics& Instance();

    // --- cortrix_rag_fusion_invocation_total (Counter, label: result) ---
    void RecordInvocation(Result result);
    uint64_t InvocationCount(Result result) const;

    // --- cortrix_rag_fusion_variant_count (Histogram, label: strategy) ---
    // Observes, per strategy, the number of variants actually produced for that
    // strategy on one query (degraded → 0). Tracks per-strategy `le` buckets
    // {1,2,3,5,10} (count-type, §topic-1 N=3 default, NS-adjustable [1-10]) + sum + count.
    void ObserveVariantCount(VariantStrategy strategy, int count);
    uint64_t VariantCountSum(VariantStrategy strategy) const;
    uint64_t VariantCountObservations(VariantStrategy strategy) const;
    /// Cumulative count at variant `le` bucket index for one strategy
    /// (0..kVariantBuckets-1 = finite bounds, kVariantBuckets = +Inf).
    uint64_t VariantCountBucket(VariantStrategy strategy, int le_index) const;

    // --- cortrix_rag_fusion_llm_latency_seconds (Histogram, label: model) ---
    // Observes the LLM variant-generation latency for `model` (a low-cardinality
    // enum string, §8). Tracks per-model `le` buckets {0.1,0.25,0.5,1,2,5,10,30}s
    // (generic duration set; §4.2 timeout_ms default 5000, typical ~500ms) +
    // sum_ms + count (rendered as seconds).
    void ObserveLlmLatency(const std::string& model, int latency_ms);

    // --- cortrix_rag_fusion_degraded_total (Counter, label: reason) ---
    void RecordDegraded(DegradeReason reason);
    uint64_t DegradedCount(DegradeReason reason) const;

    // --- cortrix_rag_fusion_token_total (Counter, label: direction) ---
    void RecordTokens(TokenDirection direction, uint64_t tokens);
    uint64_t TokenCount(TokenDirection direction) const;

    // --- cortrix_rag_fusion_rrf_fusion_duration_seconds (Histogram, no labels) ---
    // In-memory rank fusion (sub-ms..low-ms); `le` buckets {1e-4,1e-3,1e-2,0.1,1}s
    // (0.1/1/10/100ms/1s) capture the sub-ms→ms range + sum + count.
    void ObserveRrfFusionDuration(int latency_us);
    uint64_t RrfFusionDurationSumUs() const;
    uint64_t RrfFusionDurationCount() const;
    /// Cumulative count at RRF latency `le` bucket index
    /// (0..kRrfBuckets-1 = finite bounds, kRrfBuckets = +Inf).
    uint64_t RrfFusionDurationBucket(int le_index) const;

    /// Render all recorded metrics as OpenMetrics/Prometheus text exposition.
    std::string RenderOpenMetrics() const;

    /// Reset all counters/histograms (test-only — production metrics are monotonic).
    void ResetForTest();

private:
    RagFusionMetrics() = default;

    static constexpr int kResultCount = 3;       // success / degraded / disabled
    static constexpr int kDegradeReasonCount = 7;
    static constexpr int kStrategyCount = 3;     // paraphrase / subquery / reverse
    static constexpr int kDirectionCount = 2;    // input / output
    // Per-model latency: a tiny fixed-size table keyed by model name (low
    // cardinality, §8 < 50). Models seen beyond the table size fold into a shared
    // "other" bucket so the recorder stays allocation-free + bounded.
    static constexpr int kModelSlots = 8;
    // histogram `le` bucket counts (finite bounds; each array carries one extra
    // trailing +Inf slot). variant = count-type; llm latency / rrf = duration.
    static constexpr int kVariantBuckets = 5;    // 1 / 2 / 3 / 5 / 10
    static constexpr int kLatencyBuckets = 8;    // 0.1/0.25/0.5/1/2/5/10/30s
    static constexpr int kRrfBuckets = 5;        // 1e-4/1e-3/1e-2/0.1/1 s

    static int StrategyIdx(VariantStrategy s);

    std::array<std::atomic<uint64_t>, kResultCount> invocation_{};
    std::array<std::atomic<uint64_t>, kStrategyCount> variant_sum_{};
    std::array<std::atomic<uint64_t>, kStrategyCount> variant_count_{};
    // Per strategy, the variant-count `le` buckets (one extra slot for +Inf).
    std::array<std::array<std::atomic<uint64_t>, kVariantBuckets + 1>, kStrategyCount>
        variant_bkt_{};
    std::array<std::atomic<uint64_t>, kDegradeReasonCount> degraded_{};
    std::array<std::atomic<uint64_t>, kDirectionCount> token_{};
    std::atomic<uint64_t> rrf_sum_us_{0};
    std::atomic<uint64_t> rrf_count_{0};
    // RRF fusion latency `le` buckets (one extra slot for +Inf).
    std::array<std::atomic<uint64_t>, kRrfBuckets + 1> rrf_bkt_{};

    // model latency table (guarded by model_mu_; the hot atomics above are
    // lock-free, but the model string→slot mapping needs a small critical section).
    mutable std::mutex model_mu_;
    std::array<std::string, kModelSlots> model_names_{};
    std::array<std::atomic<uint64_t>, kModelSlots> model_latency_sum_ms_{};
    std::array<std::atomic<uint64_t>, kModelSlots> model_latency_count_{};
    // Per model slot, the latency `le` buckets (one extra slot for +Inf).
    std::array<std::array<std::atomic<uint64_t>, kLatencyBuckets + 1>, kModelSlots>
        model_latency_bkt_{};
};

const char* ToString(RagFusionMetrics::Result result);
const char* ToString(RagFusionMetrics::DegradeReason reason);
const char* ToString(RagFusionMetrics::TokenDirection direction);

}  // namespace cortrix::query
