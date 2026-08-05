#include "cortrix/spc/data_cleaner.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_set>
#include <utility>

#include <openssl/evp.h>

#include "cortrix/common/block_types.h"  // kFlagExtIsAnomalous (SoT)

namespace cortrix::spc {

namespace {

/// SHA-256(text) → 64-char lowercase hex (D2 exact dedup, §4.1 Step 1). Uses the
/// OpenSSL EVP the project already links (connector/file_utils.cpp uses it for
/// file hashing); cleaning needs the in-memory string form.
std::string Sha256Hex(const std::string& text) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
    EVP_DigestUpdate(ctx, text.data(), text.size());
    EVP_DigestFinal_ex(ctx, digest, &len);
    EVP_MD_CTX_free(ctx);

    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (unsigned int i = 0; i < len; ++i) {
        out.push_back(kHex[digest[i] >> 4]);
        out.push_back(kHex[digest[i] & 0x0F]);
    }
    return out;
}

/// Cosine similarity of two equal-length vectors. Returns 0 for empty / mismatched
/// dims / zero-norm (those are handled as anomalies elsewhere; here they simply
/// don't count as "similar").
double Cosine(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.empty() || a.size() != b.size()) return 0.0;
    double dot = 0.0, na = 0.0, nb = 0.0;
    for (size_t i = 0; i < a.size(); ++i) {
        dot += static_cast<double>(a[i]) * b[i];
        na += static_cast<double>(a[i]) * a[i];
        nb += static_cast<double>(b[i]) * b[i];
    }
    if (na <= 0.0 || nb <= 0.0) return 0.0;
    return dot / (std::sqrt(na) * std::sqrt(nb));
}

bool HasNaN(const std::vector<float>& v) {
    for (float x : v) if (std::isnan(x) || std::isinf(x)) return true;
    return false;
}

bool AllZero(const std::vector<float>& v) {
    if (v.empty()) return true;
    for (float x : v) if (x != 0.0f) return false;
    return true;
}

}  // namespace

const char* ToString(AnomalyReason reason) {
    switch (reason) {
        case AnomalyReason::EMPTY_CHUNK:        return "empty";
        case AnomalyReason::OVERSIZED_CHUNK:    return "oversized";
        case AnomalyReason::PARSE_FAILED:       return "parse_failed";
        case AnomalyReason::DUPLICATE_BLOCK_ID: return "duplicate_id";
        case AnomalyReason::INVALID_EMBEDDING:  return "invalid_embedding";
    }
    return "unknown";
}

DataCleaner::DataCleaner(const CleaningConfig& config,
                         std::shared_ptr<operation_log::IOperationLogger> op_logger)
    : config_(config), op_logger_(std::move(op_logger)) {}

Status DataCleaner::ValidateConfig() const {
    if (config_.dedup_similarity_threshold < 0.0 ||
        config_.dedup_similarity_threshold > 1.0) {
        return CleaningStatus(CleaningErrorCode::kDedupThresholdRange,
                              "threshold=" + std::to_string(config_.dedup_similarity_threshold));
    }
    if (config_.max_chunk_chars <= 0) {
        return CleaningStatus(CleaningErrorCode::kAnomalyConfigInvalid,
                              "max_chunk_chars=" + std::to_string(config_.max_chunk_chars));
    }
    return Status::Ok();
}

DedupResult DataCleaner::Dedup(std::vector<Block>& blocks,
                               const observability::TraceContext* /*ctx*/) {
    DedupResult result;
    if (!config_.dedup_enabled) {
        result.kept_count = static_cast<int>(blocks.size());
        return result;
    }

    for (Block& b : blocks) b.marked_for_removal = false;

    // Step 1 — exact dedup (O(N)) by SHA-256(chunk_text).
    std::unordered_set<std::string> seen_hashes;
    for (Block& b : blocks) {
        const std::string hash = Sha256Hex(b.chunk_text);
        if (!seen_hashes.insert(hash).second) {
            b.marked_for_removal = true;
            ++result.hash_dedup_count;
            result.removed_block_ids.push_back(b.id);
        }
    }

    // Step 2 — semantic dedup (O(M²) over the survivors). A candidate marked
    // similar to an earlier survivor is dropped. Vectors with NaN/zero norm yield
    // cosine 0 (never "similar"); those are caught by DetectAnomaly instead.
    std::vector<Block*> candidates;
    candidates.reserve(blocks.size());
    for (Block& b : blocks) if (!b.marked_for_removal) candidates.push_back(&b);

    for (size_t i = 0; i < candidates.size(); ++i) {
        if (candidates[i]->marked_for_removal) continue;  // dropped earlier in pass
        for (size_t j = i + 1; j < candidates.size(); ++j) {
            if (candidates[j]->marked_for_removal) continue;
            const double sim = Cosine(candidates[i]->embedding, candidates[j]->embedding);
            if (sim >= config_.dedup_similarity_threshold) {
                candidates[j]->marked_for_removal = true;
                ++result.semantic_dedup_count;
                result.removed_block_ids.push_back(candidates[j]->id);
            }
        }
    }

    // Step 3 — physical erase.
    blocks.erase(std::remove_if(blocks.begin(), blocks.end(),
                                [](const Block& b) { return b.marked_for_removal; }),
                 blocks.end());

    result.removed_count = result.hash_dedup_count + result.semantic_dedup_count;
    result.kept_count = static_cast<int>(blocks.size());
    return result;
}

AnomalyResult DataCleaner::DetectAnomaly(std::vector<Block>& blocks,
                                         const observability::TraceContext* /*ctx*/) {
    AnomalyResult result;
    if (!config_.anomaly_detection_enabled) return result;

    std::unordered_set<std::string> seen_ids;
    for (Block& b : blocks) {
        std::vector<AnomalyReason> reasons;

        if (b.chunk_text.empty()) {
            reasons.push_back(AnomalyReason::EMPTY_CHUNK);
        }
        if (static_cast<int>(b.chunk_text.size()) > config_.max_chunk_chars) {
            reasons.push_back(AnomalyReason::OVERSIZED_CHUNK);
        }
        // PARSE_FAILED: the upstream META block passes through meta.parse_status / meta.parse_failed_page
        // (F10-rev-8). Tolerate either signal.
        if (b.metadata_json.is_object()) {
            const auto& m = b.metadata_json;
            const bool parse_failed =
                (m.contains("meta.parse_status") &&
                 m["meta.parse_status"].is_string() &&
                 m["meta.parse_status"].get<std::string>() == "failed") ||
                (m.contains("meta.parse_failed_page") &&
                 !m["meta.parse_failed_page"].is_null());
            if (parse_failed) reasons.push_back(AnomalyReason::PARSE_FAILED);
        }
        if (HasNaN(b.embedding) || AllZero(b.embedding)) {
            reasons.push_back(AnomalyReason::INVALID_EMBEDDING);
        }
        if (!b.id.empty() && !seen_ids.insert(b.id).second) {
            reasons.push_back(AnomalyReason::DUPLICATE_BLOCK_ID);
        }

        if (!reasons.empty()) {
            b.flags_ext |= kFlagExtIsAnomalous;  // block-header bit2 (SoT common/block_types.h)
            std::string joined;
            for (size_t i = 0; i < reasons.size(); ++i) {
                if (i) joined.push_back(',');
                joined += ToString(reasons[i]);
                ++result.by_reason[ToString(reasons[i])];
            }
            if (!b.metadata_json.is_object()) b.metadata_json = nlohmann::json::object();
            b.metadata_json["cleaning.anomaly_reason"] = joined;
            b.metadata_json["cleaning.skip_index"] = true;
            ++result.total_marked;
        }
    }
    return result;
}

CleaningResult DataCleaner::Summarize(int input_count, const DedupResult& dedup,
                                      const AnomalyResult& anomaly) {
    CleaningResult r;
    r.chunks_input = input_count;
    r.chunks_skipped_dedup = dedup.removed_count;
    r.chunks_indexed = input_count - dedup.removed_count;  // survivors (incl. anomalous)
    r.chunks_marked_anomalous = anomaly.total_marked;
    return r;
}

void DataCleaner::RegisterPlugin(std::function<void(std::vector<Block>&)> plugin) {
    plugins_.push_back(std::move(plugin));
}

void DataCleaner::ApplyPlugins(std::vector<Block>& blocks,
                               const observability::TraceContext* /*ctx*/) {
    // Stub: run each registered plugin in order. The plugin API implements timeout
    // enforcement (CX_ERR_CLEANING_PLUGIN_TIMEOUT) + exception capture
    // (CX_ERR_CLEANING_PLUGIN_EXCEPTION) + the full ICleaningPlugin ecosystem.
    for (auto& plugin : plugins_) {
        if (plugin) plugin(blocks);
    }
}

bool ShouldSkipIndex(const Block& block) {
    if (block.flags_ext & kBlockFlagExtAnomalous) return true;
    if (block.metadata_json.is_object() &&
        block.metadata_json.contains("cleaning.skip_index")) {
        const auto& v = block.metadata_json["cleaning.skip_index"];
        return v.is_boolean() && v.get<bool>();
    }
    return false;
}

}  // namespace cortrix::spc
