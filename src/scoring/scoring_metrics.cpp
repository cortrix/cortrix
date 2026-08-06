#include <cstdint>
#include "cortrix/scoring/scoring_metrics.h"

#include <array>
#include <atomic>
#include <sstream>

namespace cortrix::scoring {

namespace {

// Histogram bucket bounds (seconds) for assign_duration (100 chunks < 1ms,
// 1000 chunks < 10ms — these are per-call, sub-ms; buckets straddle the per-batch SLAs).
constexpr size_t kNumDurBuckets = 5;
constexpr double kDurBounds[kNumDurBuckets] = {0.00001, 0.0001, 0.001, 0.01, 0.1};
constexpr const char* kDurBoundStr[kNumDurBuckets] = {"1e-05", "0.0001", "0.001", "0.01", "0.1"};

struct Hist {
    std::array<std::atomic<uint64_t>, kNumDurBuckets + 1> bkt{};  // last = +Inf
    std::atomic<uint64_t> sum_us{0};
    std::atomic<uint64_t> count{0};
    void Observe(double seconds) {
        if (seconds < 0) seconds = 0;
        size_t i = kNumDurBuckets;
        for (size_t j = 0; j < kNumDurBuckets; ++j) {
            if (seconds <= kDurBounds[j]) { i = j; break; }
        }
        bkt[i].fetch_add(1, std::memory_order_relaxed);
        count.fetch_add(1, std::memory_order_relaxed);
        sum_us.fetch_add(static_cast<uint64_t>(seconds * 1e6), std::memory_order_relaxed);
    }
    void Reset() {
        for (auto& b : bkt) b.store(0);
        sum_us.store(0);
        count.store(0);
    }
};

// Process-global atomic state. Function-local statics → one set per process, zero-init.
struct State {
    std::array<std::atomic<uint64_t>, 5> score_by_level{};  // level 0-4
    std::atomic<uint64_t> anomalous{0};
    std::atomic<uint64_t> final_score{0};
    Hist assign_dur{};
    std::array<std::atomic<uint64_t>, kScoringErrorCodeCount> error_by_code{};  // §4.4 (2 codes)
};

State& S() {
    static State s;
    return s;
}

void RenderHist(std::ostringstream& os, const char* name, const Hist& h) {
    uint64_t cum = 0;
    for (size_t i = 0; i < kNumDurBuckets; ++i) {
        cum += h.bkt[i].load(std::memory_order_relaxed);
        os << name << "_bucket{le=\"" << kDurBoundStr[i] << "\"} " << cum << "\n";
    }
    cum += h.bkt[kNumDurBuckets].load(std::memory_order_relaxed);
    os << name << "_bucket{le=\"+Inf\"} " << cum << "\n";
    os << name << "_sum " << (static_cast<double>(h.sum_us.load(std::memory_order_relaxed)) / 1e6) << "\n";
    os << name << "_count " << h.count.load(std::memory_order_relaxed) << "\n";
}

}  // namespace

ScoringMetrics& ScoringMetrics::Instance() {
    static ScoringMetrics inst;
    return inst;
}

void ScoringMetrics::RecordScore(uint8_t level) {
    if (level <= 4) S().score_by_level[level].fetch_add(1, std::memory_order_relaxed);
}

uint64_t ScoringMetrics::ScoreCount(uint8_t level) const {
    return level <= 4 ? S().score_by_level[level].load(std::memory_order_relaxed) : 0;
}

void ScoringMetrics::RecordAnomalous() {
    S().anomalous.fetch_add(1, std::memory_order_relaxed);
}

uint64_t ScoringMetrics::AnomalousCount() const {
    return S().anomalous.load(std::memory_order_relaxed);
}

void ScoringMetrics::RecordFinalScore() {
    S().final_score.fetch_add(1, std::memory_order_relaxed);
}

uint64_t ScoringMetrics::FinalScoreCount() const {
    return S().final_score.load(std::memory_order_relaxed);
}

void ScoringMetrics::ObserveAssignDuration(double seconds) {
    S().assign_dur.Observe(seconds);
}

void ScoringMetrics::RecordError(ScoringErrorCode code) {
    const auto idx = static_cast<size_t>(code);
    if (idx >= S().error_by_code.size()) return;  // (Minor) bounds guard vs an out-of-range cast
    S().error_by_code[idx].fetch_add(1, std::memory_order_relaxed);
}

uint64_t ScoringMetrics::ErrorCount(ScoringErrorCode code) const {
    const auto idx = static_cast<size_t>(code);
    if (idx >= S().error_by_code.size()) return 0;  // (Minor) bounds guard
    return S().error_by_code[idx].load(std::memory_order_relaxed);
}

std::string ScoringMetrics::Render() const {
    std::ostringstream os;

    os << "# HELP cortrix_scoring_score_total semantic_score assignment count by level.\n";
    os << "# TYPE cortrix_scoring_score_total counter\n";
    for (uint8_t lv = 0; lv <= 4; ++lv) {
        os << "cortrix_scoring_score_total{level=\"" << static_cast<int>(lv) << "\"} "
           << ScoreCount(lv) << "\n";
    }

    os << "# HELP cortrix_scoring_anomalous_blocks_total Anomalous blocks scored 0.0 (D6 sentinel).\n";
    os << "# TYPE cortrix_scoring_anomalous_blocks_total counter\n";
    os << "cortrix_scoring_anomalous_blocks_total " << AnomalousCount() << "\n";

    os << "# HELP cortrix_scoring_final_score_total ComputeFinalScore invocations (score fusion).\n";
    os << "# TYPE cortrix_scoring_final_score_total counter\n";
    os << "cortrix_scoring_final_score_total " << FinalScoreCount() << "\n";

    os << "# HELP cortrix_scoring_assign_duration_seconds AssignInitialScore call latency.\n";
    os << "# TYPE cortrix_scoring_assign_duration_seconds histogram\n";
    RenderHist(os, "cortrix_scoring_assign_duration_seconds", S().assign_dur);

    os << "# HELP cortrix_scoring_error_total scoring errors by CX_ERR_SCORING_* code (§4.4).\n";
    os << "# TYPE cortrix_scoring_error_total counter\n";
    for (int i = 0; i < kScoringErrorCodeCount; ++i) {
        const auto code = static_cast<ScoringErrorCode>(i);
        os << "cortrix_scoring_error_total{code=\"" << ScoringErrorCodeString(code) << "\"} "
           << ErrorCount(code) << "\n";
    }

    return os.str();
}

void ScoringMetrics::ResetForTest() {
    for (auto& c : S().score_by_level) c.store(0);
    S().anomalous.store(0);
    S().final_score.store(0);
    S().assign_dur.Reset();
    for (auto& c : S().error_by_code) c.store(0);
}

}  // namespace cortrix::scoring
