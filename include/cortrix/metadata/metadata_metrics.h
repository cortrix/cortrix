#pragma once
#include <cstdint>
#include <string>

namespace cortrix::metadata {

/// Metadata-block observability metrics (subsystem `f08`).
/// Naming `cortrix_metadata_<metric>_<unit>`. Self-contained dependency-free recorder
/// (same pattern as ImportMetrics / RerankerMetrics / OnnxMetrics); registering into
/// the `/metrics` scrape endpoint is wired separately.
///
/// Standalone scope: this recorder covers the generator's own metadata_block
/// lifecycle — block_generated_total / metadata_size_bytes / field_missing_total /
/// generate_duration / block_count. The PWL-coupled (block_flush_duration) and
/// doc-summary-coupled (doc_fts5_sync_total) metrics in §5.bis are emitted at the real
/// single-transaction / hybrid-fallback wiring sites → D3.5; their counters live here
/// so the call sites are a one-line add when that wiring lands, but are not driven
/// standalone.
///
/// High-cardinality labels (doc_id / ns_id) are forbidden (OBS_SPEC §3.2 deny list);
/// per-NS data goes through the OBS_SPEC §3.4 system stats API, not metric labels.
class MetadataMetrics {
public:
    /// status label for cortrix_metadata_block_generated_total (detailed design §5.bis).
    enum class GenStatus { kSuccess = 0, kPartialWarning, kFailed };
    /// source label for cortrix_metadata_block_generated_total.
    enum class GenSource { kApiUpload = 0, kBatchImport, kReUpload };

    /// Process-wide instance (metrics are global counters/gauges).
    static MetadataMetrics& Instance();

    // cortrix_metadata_block_generated_total (Counter, labels: status, source).
    void RecordGenerated(GenStatus status, GenSource source);
    uint64_t GeneratedCount(GenStatus status, GenSource source) const;

    // cortrix_metadata_block_count_current (Gauge — row count of the metadata_blocks table). Set by the
    // store layer; standalone tests drive it directly.
    void SetBlockCount(int64_t count);
    int64_t BlockCount() const;

    // cortrix_metadata_field_missing_total (Counter, label: field_name — controlled enum,
    // a subset of v1_immutable_fields). field_name is taken verbatim; the generator only
    // passes schema field names, so cardinality stays bounded.
    void RecordFieldMissing(const std::string& field_name);
    uint64_t FieldMissingCount(const std::string& field_name) const;

    // cortrix_metadata_size_bytes (Histogram — per-entry byte-size distribution of metadata_json).
    void ObserveMetadataSize(double bytes);

    // cortrix_metadata_block_generate_duration_seconds (Histogram — total time for block_text assembly +
    // JSON serialization [+ SQLite insert in D3.5]; §5.bis SLA P95 ≤ 50ms).
    void ObserveGenerateDuration(double seconds);

    /// Render the current values as OpenMetrics text (what the endpoint will
    /// serve). Stable metric names + HELP/TYPE lines.
    std::string Render() const;

    /// Reset all values (test helper — metrics are otherwise process-global).
    void ResetForTest();

private:
    MetadataMetrics() = default;
};

const char* ToString(MetadataMetrics::GenStatus status);
const char* ToString(MetadataMetrics::GenSource source);

}  // namespace cortrix::metadata
