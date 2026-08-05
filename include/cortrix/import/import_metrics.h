#pragma once
#include <cstdint>
#include <string>

namespace cortrix::import {

/// Database-import observability metrics (subsystem `f16a`).
/// Naming `cortrix_f16a_<metric>_<unit>`. Self-contained dependency-free recorder
/// (same pattern as RerankerMetrics / OnnxMetrics); registering into the
/// `/metrics` scrape endpoint is cross-Feature wiring → D3.5.
///
/// v1.0.2 reverse-fix (§5.5 note): the high-cardinality labels `namespace` (on
/// rows_imported) and `tenant_id` (on connections_active) were REMOVED — they are on
/// the OBS_SPEC §3.2 absolute-deny list. per-tenant / per-namespace breakdown goes
/// through the OBS_SPEC §3.4 per-tenant API, not metric labels.
class ImportMetrics {
public:
    /// status label for cortrix_f16a_imports_total.
    enum class ImportOutcome { kSuccess = 0, kFailed, kCancelled };
    /// query_type label for cortrix_f16a_query_duration_seconds.
    enum class QueryType { kTableFilter = 0, kCustomSql };

    /// Process-wide instance (metrics are global counters/gauges).
    static ImportMetrics& Instance();

    // cortrix_f16a_imports_total (Counter, label: status).
    void RecordImport(ImportOutcome outcome);
    uint64_t ImportsCount(ImportOutcome outcome) const;

    // cortrix_f16a_rows_imported_total (Counter, no label — §5.5 note removed namespace).
    void AddRowsImported(int64_t rows);
    uint64_t RowsImportedTotal() const;

    // cortrix_f16a_connections_active (Gauge, no label — §5.5 note removed tenant_id).
    void IncConnectionsActive();
    void DecConnectionsActive();
    int64_t ConnectionsActive() const;

    // cortrix_f16a_tasks_queue_depth (Gauge).
    void SetQueueDepth(int64_t depth);
    int64_t QueueDepth() const;

    // cortrix_f16a_import_duration_seconds (Histogram, label: text_strategy).
    // text_strategy is "per_row" / "merge" (§5.5; any other value buckets as per_row).
    void ObserveImportDuration(const std::string& text_strategy, double seconds);

    // cortrix_f16a_query_duration_seconds (Histogram, label: query_type).
    void ObserveQueryDuration(QueryType query_type, double seconds);

    /// Render the current values as OpenMetrics text (what the endpoint will
    /// serve). Stable metric names + HELP/TYPE lines.
    std::string Render() const;

    /// Reset all values (test helper — metrics are otherwise process-global).
    void ResetForTest();

private:
    ImportMetrics() = default;
};

const char* ToString(ImportMetrics::ImportOutcome outcome);
const char* ToString(ImportMetrics::QueryType type);

}  // namespace cortrix::import
