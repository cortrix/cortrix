#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "cortrix/catalog/i_bloom_filter.h"
#include "cortrix/common/result.h"
#include "cortrix/common/status.h"

typedef struct sqlite3 sqlite3;

namespace cortrix::catalog {

/// Standard Bloom filter (default: 100MB / 1% FP / ~100M file_hash). Bit
/// array + k hash functions derived from two 64-bit base hashes
/// (Kirsch-Mitzenmacher double hashing). Optimal m (bits) and k are computed
/// from capacity + target FP rate.
///
/// Thread-safety: Add takes a mutex (bit writes); MightContain is lock-free over
/// atomic words. Runtime stats use atomics.
class BloomFilter : public IBloomFilter {
public:
    /// `capacity_bytes` sizes the bit array (§7: 100MB). `target_fp_rate` (§7:
    /// 0.01) sets k. Capacity in expected items is derived from m and the target.
    BloomFilter(size_t capacity_bytes, double target_fp_rate);

    Status Add(const std::string& file_hash) override;
    bool MightContain(const std::string& file_hash) const override;
    size_t EstimatedCount() const override;
    double EstimatedFalsePositiveRate() const override;

    bool IsReady() const override;
    float GetLoadProgress() const override;
    std::optional<int64_t> LastRebuildAt() const override;

    FalsePositiveStats GetFalsePositiveStats() const override;
    void ReportFalsePositive() override;

    /// Startup recovery (§7.2): full-scan catalog.file_locations and Add() every
    /// file_hash, then mark ready. Borrows the catalog sqlite3 handle.
    ///
    /// SPEC DEVIATION (flagged): the design spells this LoadFromCatalog(INSRouter*),
    /// but INSRouter (S2.1) is NS-centric and exposes no file_hash enumeration —
    /// the BF's data source is the dedup table file_locations, not NS routing.
    /// Taking the sqlite3 handle (as CatalogDb/Default routers do) is the correct
    /// Layer-0 source. Querying file_locations directly keeps the NS-router
    /// contract surface (frozen by the L0 review) unpolluted by dedup concerns.
    Status LoadFromCatalog(sqlite3* db);

    /// Hooks for tests / non-DB callers: set readiness explicitly.
    void MarkReady(int64_t now_ms);

private:
    // Two 64-bit base hashes of the key (FNV-1a over two salts) → k indices.
    void HashKey(const std::string& key, uint64_t* h1, uint64_t* h2) const;
    bool TestBit(size_t bit) const;
    void SetBit(size_t bit);

    size_t num_bits_;                       // m (rounded to a multiple of 64)
    int    num_hashes_;                     // k
    std::vector<std::atomic<uint64_t>> words_;  // bit array, 64 bits/word

    std::atomic<size_t> added_count_{0};
    std::atomic<bool> ready_{false};
    std::atomic<float> load_progress_{0.0f};
    std::atomic<int64_t> last_rebuild_ms_{-1};

    // Runtime counters: updated from the const MightContain() (a positive
    // hit bumps total_checks_), so mutable — they are observability state, not
    // part of the filter's logical value.
    mutable std::atomic<uint64_t> total_checks_{0};         // metrics counter
    mutable std::atomic<uint64_t> false_positive_count_{0}; // metrics counter

    mutable std::mutex add_mu_;             // serialize Add bit-writes
};

}  // namespace cortrix::catalog
