#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

namespace cortrix::deploy {

/// The deployment/system metrics (observability
/// §2.3 subsystems `disk` / system). Self-contained dependency-free recorder
/// (same pattern as ScoringMetrics / F42Metrics): a process-wide singleton of
/// atomic gauges + an OpenMetrics text renderer that the :9091 endpoint serves.
///
/// 🚨 Cardinality control (OBS_SPEC §3.2): the gauges here are global (no
/// per-NS / per-doc labels). The Bloom Filter gauges carry only the fixed
/// subsystem="catalog" label.
///
/// Scope (D3 standalone): the gauges this recorder owns directly are
///   cortrix_disk_usage_ratio          (gauge — fed by DiskMonitor)
///   cortrix_shutdown_status           (gauge 0/1/2 — fed by GracefulShutdown)
///   cortrix_uptime_seconds            (gauge — derived from the process start)
///   cortrix_build_info{version,...}   (info gauge, value 1)
/// plus the 4 Bloom Filter gauges, which are *rendered from* an
/// IBloomFilter the metrics server is given (read-through, not stored here).
/// The remaining ~21 subsystem metrics in §5.3 are owned by their own Feature
/// recorders (ScoringMetrics, F42Metrics, ...); aggregating all of them into one
/// /metrics body is cross-Feature wiring → D3.5 (see metrics_server.h).
class DeployMetrics {
public:
    /// Process-wide instance (metrics are global gauges).
    static DeployMetrics& Instance();

    // --- cortrix_disk_usage_ratio (gauge) ---
    // (total - free) / total of the data volume, in [0,1]. Set by DiskMonitor.
    void SetDiskUsageRatio(double ratio);
    double DiskUsageRatio() const;

    // --- cortrix_shutdown_status (gauge 0/1/2) ---
    // 0 = running, 1 = shutting_down, 2 = forced (timed out with persisted tasks).
    void SetShutdownStatus(int status);
    int ShutdownStatus() const;

    // --- cortrix_build_info{version,git_commit,build_date} (info gauge, value 1) ---
    // Set once at startup. Stable label set; the metric value is always 1.
    void SetBuildInfo(const std::string& version,
                      const std::string& git_commit,
                      const std::string& build_date);

    // --- cortrix_uptime_seconds (gauge) ---
    // Marks the process start; Render() emits the live elapsed seconds.
    void MarkStart();

    /// Render the gauges this recorder owns as OpenMetrics text (HELP/TYPE +
    /// value lines). Does NOT include the Bloom Filter gauges — those are
    /// rendered separately by RenderBloomFilterMetrics() because they read
    /// through a live IBloomFilter. Stable metric names.
    std::string Render() const;

    /// Reset all gauges (test helper — metrics are otherwise process-global).
    void ResetForTest();

private:
    DeployMetrics() = default;
};

/// Read-through renderer for the 4 Bloom Filter gauges, kept separate
/// from DeployMetrics because they reflect a live filter rather than stored
/// state. Given accessors for the four values (so it does not hard-depend on the
/// catalog headers — the metrics server binds these from the real IBloomFilter,
/// converting LastRebuildAt() unix-ms → epoch seconds), emit:
///   cortrix_bloom_filter_estimated_count{subsystem="catalog"}
///   cortrix_bloom_filter_false_positive_rate{subsystem="catalog"}
///   cortrix_bloom_filter_last_rebuild_ts{subsystem="catalog"}   (epoch second)
///   cortrix_bloom_filter_ready{subsystem="catalog"}             (0/1)
struct BloomFilterMetricSource {
    std::function<uint64_t()> estimated_count;
    std::function<double()>   false_positive_rate;
    std::function<int64_t()>  last_rebuild_epoch_sec;  ///< -1 if never rebuilt → emit 0
    std::function<bool()>     ready;
    std::string subsystem = "catalog";
};

/// Render the 4 BF gauges from `src` (any null accessor is skipped). Used by the
/// /metrics endpoint; standalone-testable with lambda sources.
std::string RenderBloomFilterMetrics(const BloomFilterMetricSource& src);

}  // namespace cortrix::deploy
