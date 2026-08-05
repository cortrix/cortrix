#pragma once
// Memory Scoring & Classified Decay
// Pure-compute scoring component for the Memory Search pipeline.
//
// Decay rules:
//   - fact / preference : immune to time decay -> final = raw_score
//   - event / "" / unknown : exponential decay -> final = raw_score * exp(-lambda * age_days)
//   - status == "invalidated" : filtered out by ScoreAndRank (unless include_invalidated)
//
// MemoryScorer is an internal implementation class (NOT an I* interface);
// its sole consumer is MemorySearcher. It performs no I/O, so it is fully
// unit-testable in isolation (design § 9.3).
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace cortrix {

/// Decay configuration. Defaults come from the global GUC (config.yaml
/// memory.decay.*); see design § 2.5. D5 lock: V1.0 is global-GUC only.
struct MemoryDecayConfig {
    double lambda = 0.01;        // decay coefficient (default 0.01, half-life ~70d)
    double min_score = 0.0;      // decay floor (default 0, old events sink naturally)
    bool llm_available = false;  // whether an LLM is configured (logging/monitoring only)
};

/// Pipeline intermediate: a memory candidate fed to the scorer.
/// Fields originate from the unified read pipeline (raw_score) and the
/// block's metadata_json (memory_type / status / created_at).
struct MemoryCandidate {
    uint64_t block_id = 0;
    std::string content;
    std::string memory_type;     // "fact" / "preference" / "event" / "" (from metadata_json)
    std::string status;          // "active" / "tentative" / "invalidated" (from metadata_json)
    double raw_score = 0.0;      // semantic similarity (Reranker score)
    int64_t created_at = 0;      // Unix timestamp (blocks.created_at)
};

/// Scored memory with transparent score breakdown (design § 2.1).
/// Field set is append-only across V1.0 -> Phase 2 (§ 10.2 stability promise).
struct ScoredMemory {
    uint64_t block_id = 0;
    std::string content;
    std::string memory_type;     // "fact" / "preference" / "event" / ""
    std::string status;          // "active" / "tentative" / "invalidated"
    double raw_score = 0.0;      // semantic similarity (from unified read pipeline)
    double decay_factor = 0.0;   // fact/preference=1.0, event=exp(-lambda*age)
    double final_score = 0.0;    // raw_score * decay_factor (clamped to >= 0)
    int64_t created_at = 0;      // memory creation time (Unix timestamp)
    int64_t age_days = 0;        // days since creation (clamped to >= 0)
};

class MemoryScorer {
public:
    explicit MemoryScorer(const MemoryDecayConfig& config);

    /// Score a single memory (decay applied per memory_type).
    ScoredMemory Score(uint64_t block_id,
                       const std::string& content,
                       const std::string& memory_type,
                       const std::string& status,
                       double raw_score,
                       int64_t created_at,
                       int64_t now) const;

    /// Convenience overload scoring directly from a MemoryCandidate.
    ScoredMemory Score(const MemoryCandidate& cand, int64_t now) const;

    /// Batch score + filter invalidated + rank by final_score (desc) + truncate.
    /// @param include_invalidated  D4 lock: default false strictly filters
    ///                             status=="invalidated"; true keeps them
    ///                             (audit / debug / the memory-history page).
    /// Same final_score keeps the original (raw_score) order via stable_sort.
    std::vector<ScoredMemory> ScoreAndRank(const std::vector<MemoryCandidate>& candidates,
                                           int64_t now,
                                           int top_k,
                                           bool include_invalidated = false) const;

    /// True iff the memory_type is immune to time decay (fact / preference).
    /// D3 lock: unknown (non-"event", non-"") types emit a WARN and fall back
    /// to event decay (returns false).
    bool IsDecayImmune(const std::string& memory_type) const;

    /// Decay factor exp(-lambda * age_days), clamped to >= min_score.
    /// age_days <= 0 -> 1.0 (today or future: no decay).
    double ComputeDecayFactor(int64_t age_days) const;

    /// Config accessor (debug / logging).
    const MemoryDecayConfig& GetConfig() const { return config_; }

private:
    MemoryDecayConfig config_;
    static const std::unordered_set<std::string> kImmuneTypes;  // {fact, preference}
};

}  // namespace cortrix
