#pragma once
#include <cstdint>
#include <optional>
#include <string>

#include "cortrix/common/result.h"
#include "cortrix/common/status.h"

namespace cortrix::catalog {

/// Probabilistic dedup accelerator. A fast negative filter in
/// front of the catalog dedup lookup: MightContain()==false means the file_hash
/// is definitely absent (skip the SQLite query); ==true means "maybe" (verify
/// against file_locations — catalog stays the SoT).
///
/// Error model (F-FREEZE-1): Add returns Status (void-fallible). Reads are plain
/// (a filter check never errors — an unready filter answers conservatively true).
class IBloomFilter {
public:
    virtual ~IBloomFilter() = default;

    /// Record a file_hash as present. Status (void-fallible) per CONVENTIONS
    virtual Status Add(const std::string& file_hash) = 0;

    /// True = possibly present (caller must verify); false = definitely absent.
    /// While not ready (startup rebuild in progress) this returns true
    /// conservatively so correctness never depends on the filter.
    virtual bool MightContain(const std::string& file_hash) const = 0;

    /// Number of distinct Add()s observed (approximate — counts Add calls).
    virtual size_t EstimatedCount() const = 0;

    /// Theoretical false-positive rate for the current fill (static formula).
    virtual double EstimatedFalsePositiveRate() const = 0;

    // ----- Agent-friendly status (topic 4.x) -----
    virtual bool IsReady() const = 0;                       ///< startup rebuild done?
    virtual float GetLoadProgress() const = 0;             ///< rebuild progress 0.0–1.0
    virtual std::optional<int64_t> LastRebuildAt() const = 0;  ///< unix ms, null if never

    // ----- OpenMetrics runtime FPR -----
    struct FalsePositiveStats {
        uint64_t total_checks;          ///< MightContain calls that hit (true)
        uint64_t false_positive_count;  ///< of those, verified absent afterwards
        double   runtime_fpr;           ///< false_positive_count / total_checks
    };
    /// Runtime FP stats for the `cortrix_bloom_filter_*` metrics. The caller
    /// reports a verified false positive via ReportFalsePositive() (below) so the
    /// filter can compute the observed rate (distinct from the theoretical one).
    virtual FalsePositiveStats GetFalsePositiveStats() const = 0;

    /// Caller feedback: a MightContain()==true that the catalog then proved
    /// absent (a real false positive). Drives GetFalsePositiveStats().
    virtual void ReportFalsePositive() = 0;
};

}  // namespace cortrix::catalog
