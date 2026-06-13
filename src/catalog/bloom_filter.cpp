#include "cortrix/catalog/bloom_filter.h"

#include <sqlite3.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>

namespace cortrix::catalog {

namespace {

int64_t WallMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// FNV-1a 64-bit over (salt || key) — cheap, well-distributed for two base hashes.
uint64_t Fnv1a(uint64_t salt, const std::string& key) {
    uint64_t h = 1469598103934665603ULL ^ salt;
    for (unsigned char c : key) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    // final avalanche so low bits mix well
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    return h;
}

}  // namespace

BloomFilter::BloomFilter(size_t capacity_bytes, double target_fp_rate) {
    // m = bits available; round up to a whole number of 64-bit words.
    size_t bits = (capacity_bytes == 0 ? 1 : capacity_bytes) * 8;
    size_t words = (bits + 63) / 64;
    if (words == 0) words = 1;
    num_bits_ = words * 64;
    words_ = std::vector<std::atomic<uint64_t>>(words);  // value-init → all 0

    // k from the target FP rate: k = round(-log2(p)), the optimal count for a
    // target p (clamped to [1, 30]). §7 p=0.01 → k≈7.
    double p = (target_fp_rate > 0.0 && target_fp_rate < 1.0) ? target_fp_rate : 0.01;
    int k = static_cast<int>(std::lround(-std::log2(p)));
    if (k < 1) k = 1;
    if (k > 30) k = 30;
    num_hashes_ = k;
}

void BloomFilter::HashKey(const std::string& key, uint64_t* h1, uint64_t* h2) const {
    *h1 = Fnv1a(0x9e3779b97f4a7c15ULL, key);
    *h2 = Fnv1a(0xc2b2ae3d27d4eb4fULL, key);
    if (*h2 == 0) *h2 = 1;  // ensure the stride is non-zero
}

bool BloomFilter::TestBit(size_t bit) const {
    const size_t w = bit / 64;
    const uint64_t mask = uint64_t(1) << (bit % 64);
    return (words_[w].load(std::memory_order_relaxed) & mask) != 0;
}

void BloomFilter::SetBit(size_t bit) {
    const size_t w = bit / 64;
    const uint64_t mask = uint64_t(1) << (bit % 64);
    words_[w].fetch_or(mask, std::memory_order_relaxed);
}

Status BloomFilter::Add(const std::string& file_hash) {
    uint64_t h1, h2;
    HashKey(file_hash, &h1, &h2);
    {
        std::lock_guard<std::mutex> lock(add_mu_);
        for (int i = 0; i < num_hashes_; ++i) {
            const uint64_t combined = h1 + static_cast<uint64_t>(i) * h2;
            SetBit(combined % num_bits_);
        }
    }
    added_count_.fetch_add(1, std::memory_order_relaxed);
    return Status::Ok();
}

bool BloomFilter::MightContain(const std::string& file_hash) const {
    // Conservative while rebuilding (§9.2): claim "maybe present" so the caller
    // falls back to the catalog and correctness never hinges on a partial filter.
    if (!ready_.load(std::memory_order_acquire)) {
        return true;
    }
    uint64_t h1, h2;
    HashKey(file_hash, &h1, &h2);
    for (int i = 0; i < num_hashes_; ++i) {
        const uint64_t combined = h1 + static_cast<uint64_t>(i) * h2;
        if (!TestBit(combined % num_bits_)) {
            return false;  // definitely absent
        }
    }
    // A positive hit — counted for the runtime FPR denominator (§3.4 F24).
    total_checks_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

size_t BloomFilter::EstimatedCount() const {
    return added_count_.load(std::memory_order_relaxed);
}

double BloomFilter::EstimatedFalsePositiveRate() const {
    // Theoretical (1 - e^{-kn/m})^k for the current fill n.
    const double n = static_cast<double>(added_count_.load(std::memory_order_relaxed));
    const double m = static_cast<double>(num_bits_);
    const double k = static_cast<double>(num_hashes_);
    if (n == 0.0) return 0.0;
    return std::pow(1.0 - std::exp(-k * n / m), k);
}

bool BloomFilter::IsReady() const {
    return ready_.load(std::memory_order_acquire);
}

float BloomFilter::GetLoadProgress() const {
    return load_progress_.load(std::memory_order_relaxed);
}

std::optional<int64_t> BloomFilter::LastRebuildAt() const {
    const int64_t v = last_rebuild_ms_.load(std::memory_order_relaxed);
    if (v < 0) return std::nullopt;
    return v;
}

IBloomFilter::FalsePositiveStats BloomFilter::GetFalsePositiveStats() const {
    FalsePositiveStats s;
    s.total_checks = total_checks_.load(std::memory_order_relaxed);
    s.false_positive_count = false_positive_count_.load(std::memory_order_relaxed);
    s.runtime_fpr = s.total_checks > 0
                        ? static_cast<double>(s.false_positive_count) /
                              static_cast<double>(s.total_checks)
                        : 0.0;
    return s;
}

void BloomFilter::ReportFalsePositive() {
    false_positive_count_.fetch_add(1, std::memory_order_relaxed);
}

void BloomFilter::MarkReady(int64_t now_ms) {
    load_progress_.store(1.0f, std::memory_order_relaxed);
    last_rebuild_ms_.store(now_ms, std::memory_order_relaxed);
    ready_.store(true, std::memory_order_release);
}

Status BloomFilter::LoadFromCatalog(sqlite3* db) {
    if (db == nullptr) {
        return Status::InvalidArgument("BloomFilter::LoadFromCatalog: null db");
    }
    ready_.store(false, std::memory_order_release);
    load_progress_.store(0.0f, std::memory_order_relaxed);

    // Total for progress reporting (best-effort; absent → progress jumps to 1).
    int64_t total = 0;
    {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM file_locations", -1, &stmt,
                               nullptr) == SQLITE_OK) {
            if (sqlite3_step(stmt) == SQLITE_ROW) total = sqlite3_column_int64(stmt, 0);
            sqlite3_finalize(stmt);
        }
    }

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT file_hash FROM file_locations", -1, &stmt,
                           nullptr) != SQLITE_OK) {
        return Status::Internal(std::string("LoadFromCatalog prepare: ") +
                                sqlite3_errmsg(db));
    }
    int64_t loaded = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* t = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        if (t) Add(std::string(t));
        ++loaded;
        if (total > 0) {
            load_progress_.store(static_cast<float>(loaded) / static_cast<float>(total),
                                 std::memory_order_relaxed);
        }
    }
    sqlite3_finalize(stmt);

    MarkReady(WallMs());
    return Status::Ok();
}

}  // namespace cortrix::catalog
