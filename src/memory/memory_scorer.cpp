#include <cstdint>
#include "cortrix/memory_scorer.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>

namespace cortrix {

// Immune types: fact / preference never decay (design).
const std::unordered_set<std::string> MemoryScorer::kImmuneTypes = {
    "fact", "preference"};

MemoryScorer::MemoryScorer(const MemoryDecayConfig& config) : config_(config) {}

bool MemoryScorer::IsDecayImmune(const std::string& memory_type) const {
    if (kImmuneTypes.count(memory_type) > 0) {
        return true;
    }
    // lock: defensively treat unknown types as event + WARN log.
    // "event" and "" are known degradation cases and do NOT warn.
    if (memory_type != "event" && !memory_type.empty()) {
        spdlog::warn("Unknown memory_type: '{}', falling back to event decay",
                     memory_type);
    }
    return false;
}

double MemoryScorer::ComputeDecayFactor(int64_t age_days) const {
    if (age_days <= 0) {
        return 1.0;  // today or future -> no decay
    }
    double factor = std::exp(-config_.lambda * static_cast<double>(age_days));
    return std::max(factor, config_.min_score);  // not below the floor
}

ScoredMemory MemoryScorer::Score(uint64_t block_id,
                                 const std::string& content,
                                 const std::string& memory_type,
                                 const std::string& status,
                                 double raw_score,
                                 int64_t created_at,
                                 int64_t now) const {
    ScoredMemory m;
    m.block_id = block_id;
    m.content = content;
    m.memory_type = memory_type;
    m.status = status;
    m.raw_score = raw_score;
    m.created_at = created_at;

    // age_days = floor((now - created_at) / 86400); clamp negatives / missing to 0.
    int64_t age_days = 0;
    if (created_at > 0 && now > created_at) {
        age_days = (now - created_at) / 86400;  // integer floor for positive values
    }
    m.age_days = age_days;

    // Decay factor: immune types -> 1.0; otherwise exponential decay.
    m.decay_factor = IsDecayImmune(memory_type) ? 1.0 : ComputeDecayFactor(age_days);

    // final = raw * decay, clamped to >= 0 (negative raw_score -> 0).
    double final_score = raw_score * m.decay_factor;
    m.final_score = final_score < 0.0 ? 0.0 : final_score;

    return m;
}

ScoredMemory MemoryScorer::Score(const MemoryCandidate& cand, int64_t now) const {
    return Score(cand.block_id, cand.content, cand.memory_type, cand.status,
                 cand.raw_score, cand.created_at, now);
}

std::vector<ScoredMemory> MemoryScorer::ScoreAndRank(
    const std::vector<MemoryCandidate>& candidates,
    int64_t now,
    int top_k,
    bool include_invalidated) const {
    std::vector<ScoredMemory> results;
    results.reserve(candidates.size());

    for (const auto& cand : candidates) {
        // conditional filter: drop invalidated unless explicitly included.
        if (cand.status == "invalidated" && !include_invalidated) {
            continue;
        }
        results.push_back(Score(cand, now));
    }

    // Rank by final_score desc; stable_sort keeps original order on ties
    // (deterministic ordering — design boundary "same final_score").
    std::stable_sort(results.begin(), results.end(),
                     [](const ScoredMemory& a, const ScoredMemory& b) {
                         return a.final_score > b.final_score;
                     });

    // Truncate to top_k (top_k <= 0 -> empty).
    if (top_k <= 0) {
        results.clear();
    } else if (static_cast<int>(results.size()) > top_k) {
        results.resize(static_cast<size_t>(top_k));
    }

    return results;
}

}  // namespace cortrix
