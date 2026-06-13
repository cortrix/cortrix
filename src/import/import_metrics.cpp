#include "cortrix/import/import_metrics.h"

#include <array>
#include <atomic>
#include <sstream>

namespace cortrix::import {

namespace {

// Histogram bucket bounds (seconds) shared by import_duration / query_duration.
constexpr size_t kNumDurBuckets = 7;
constexpr double kDurBounds[kNumDurBuckets] = {0.05, 0.25, 1.0, 5.0, 30.0, 120.0, 300.0};
constexpr const char* kDurBoundStr[kNumDurBuckets] = {"0.05", "0.25", "1", "5", "30", "120", "300"};

// Minimal lock-free Prometheus histogram (non-cumulative buckets; rendered cumulative).
struct Hist {
    std::array<std::atomic<uint64_t>, kNumDurBuckets + 1> bkt{};  // last = +Inf
    std::atomic<uint64_t> sum_us{0};
    std::atomic<uint64_t> count{0};
    void Observe(double seconds) {
        if (seconds < 0) seconds = 0;
        size_t i = kNumDurBuckets;  // +Inf default
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

// Process-global atomic state (counters/gauges/histograms). Function-local statics
// → one set per process, zero-initialized.
struct State {
    std::array<std::atomic<uint64_t>, 3> imports{};   // indexed by ImportOutcome
    std::atomic<uint64_t> rows_imported{0};
    std::atomic<int64_t> connections_active{0};
    std::atomic<int64_t> queue_depth{0};
    std::array<Hist, 2> import_dur{};   // [0]=per_row, [1]=merge
    std::array<Hist, 2> query_dur{};    // indexed by QueryType
};

State& S() {
    static State s;
    return s;
}

// Render one histogram in OpenMetrics format (cumulative le buckets + _sum + _count).
void RenderHist(std::ostringstream& os, const char* name, const char* label_name,
                const char* label_val, const Hist& h) {
    uint64_t cum = 0;
    for (size_t i = 0; i < kNumDurBuckets; ++i) {
        cum += h.bkt[i].load(std::memory_order_relaxed);
        os << name << "_bucket{" << label_name << "=\"" << label_val
           << "\",le=\"" << kDurBoundStr[i] << "\"} " << cum << "\n";
    }
    cum += h.bkt[kNumDurBuckets].load(std::memory_order_relaxed);
    os << name << "_bucket{" << label_name << "=\"" << label_val << "\",le=\"+Inf\"} " << cum << "\n";
    os << name << "_sum{" << label_name << "=\"" << label_val << "\"} "
       << (static_cast<double>(h.sum_us.load(std::memory_order_relaxed)) / 1e6) << "\n";
    os << name << "_count{" << label_name << "=\"" << label_val << "\"} "
       << h.count.load(std::memory_order_relaxed) << "\n";
}

}  // namespace

const char* ToString(ImportMetrics::ImportOutcome outcome) {
    switch (outcome) {
        case ImportMetrics::ImportOutcome::kSuccess:   return "success";
        case ImportMetrics::ImportOutcome::kFailed:    return "failed";
        case ImportMetrics::ImportOutcome::kCancelled: return "cancelled";
    }
    return "success";
}

const char* ToString(ImportMetrics::QueryType type) {
    switch (type) {
        case ImportMetrics::QueryType::kTableFilter: return "table_filter";
        case ImportMetrics::QueryType::kCustomSql:   return "custom_sql";
    }
    return "table_filter";
}

ImportMetrics& ImportMetrics::Instance() {
    static ImportMetrics inst;
    return inst;
}

void ImportMetrics::RecordImport(ImportOutcome outcome) {
    S().imports[static_cast<size_t>(outcome)].fetch_add(1, std::memory_order_relaxed);
}

uint64_t ImportMetrics::ImportsCount(ImportOutcome outcome) const {
    return S().imports[static_cast<size_t>(outcome)].load(std::memory_order_relaxed);
}

void ImportMetrics::AddRowsImported(int64_t rows) {
    if (rows > 0) S().rows_imported.fetch_add(static_cast<uint64_t>(rows), std::memory_order_relaxed);
}

uint64_t ImportMetrics::RowsImportedTotal() const {
    return S().rows_imported.load(std::memory_order_relaxed);
}

void ImportMetrics::IncConnectionsActive() {
    S().connections_active.fetch_add(1, std::memory_order_relaxed);
}

void ImportMetrics::DecConnectionsActive() {
    S().connections_active.fetch_sub(1, std::memory_order_relaxed);
}

int64_t ImportMetrics::ConnectionsActive() const {
    return S().connections_active.load(std::memory_order_relaxed);
}

void ImportMetrics::SetQueueDepth(int64_t depth) {
    S().queue_depth.store(depth, std::memory_order_relaxed);
}

int64_t ImportMetrics::QueueDepth() const {
    return S().queue_depth.load(std::memory_order_relaxed);
}

void ImportMetrics::ObserveImportDuration(const std::string& text_strategy, double seconds) {
    S().import_dur[text_strategy == "merge" ? 1 : 0].Observe(seconds);
}

void ImportMetrics::ObserveQueryDuration(QueryType query_type, double seconds) {
    S().query_dur[static_cast<size_t>(query_type)].Observe(seconds);
}

std::string ImportMetrics::Render() const {
    std::ostringstream os;
    os << "# HELP cortrix_f16a_imports_total Total DB import tasks by status.\n";
    os << "# TYPE cortrix_f16a_imports_total counter\n";
    for (auto o : {ImportOutcome::kSuccess, ImportOutcome::kFailed, ImportOutcome::kCancelled}) {
        os << "cortrix_f16a_imports_total{status=\"" << ToString(o) << "\"} "
           << ImportsCount(o) << "\n";
    }
    os << "# HELP cortrix_f16a_rows_imported_total Total rows imported.\n";
    os << "# TYPE cortrix_f16a_rows_imported_total counter\n";
    os << "cortrix_f16a_rows_imported_total " << RowsImportedTotal() << "\n";
    os << "# HELP cortrix_f16a_connections_active Active registered DB connections.\n";
    os << "# TYPE cortrix_f16a_connections_active gauge\n";
    os << "cortrix_f16a_connections_active " << ConnectionsActive() << "\n";
    os << "# HELP cortrix_f16a_tasks_queue_depth Import task queue depth.\n";
    os << "# TYPE cortrix_f16a_tasks_queue_depth gauge\n";
    os << "cortrix_f16a_tasks_queue_depth " << QueueDepth() << "\n";
    os << "# HELP cortrix_f16a_import_duration_seconds DB import wall-clock by text strategy.\n";
    os << "# TYPE cortrix_f16a_import_duration_seconds histogram\n";
    RenderHist(os, "cortrix_f16a_import_duration_seconds", "text_strategy", "per_row", S().import_dur[0]);
    RenderHist(os, "cortrix_f16a_import_duration_seconds", "text_strategy", "merge", S().import_dur[1]);
    os << "# HELP cortrix_f16a_query_duration_seconds External-PG query wall-clock by query type.\n";
    os << "# TYPE cortrix_f16a_query_duration_seconds histogram\n";
    RenderHist(os, "cortrix_f16a_query_duration_seconds", "query_type", "table_filter", S().query_dur[0]);
    RenderHist(os, "cortrix_f16a_query_duration_seconds", "query_type", "custom_sql", S().query_dur[1]);
    return os.str();
}

void ImportMetrics::ResetForTest() {
    for (auto& c : S().imports) c.store(0);
    S().rows_imported.store(0);
    S().connections_active.store(0);
    S().queue_depth.store(0);
    for (auto& h : S().import_dur) h.Reset();
    for (auto& h : S().query_dur) h.Reset();
}

}  // namespace cortrix::import
