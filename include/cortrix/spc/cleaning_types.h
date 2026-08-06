#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace cortrix::spc {

/// The data cleaner's in-memory block view. The SPC Pipeline assembles richer
/// CortrixBlock BLOBs (header + payload); cleaning operates *before* assembly on
/// this lightweight view carrying exactly what dedup + anomaly detection need:
/// id, chunk text, the upstream OnnxEmbedder embedding (D3 — zero-cost reuse),
/// the JSONB metadata namespace, and the flags_ext byte to mark anomalies.
///
/// NOTE (D3.5): the design's §4.3 SpcPipeline integration maps
/// ChunkResult + EmbeddingResult (→ CortrixBlock) onto this view and back. That
/// The wiring and the unified post-chunk Block land separately — cleaning is standalone
/// defines and operates on this contract view.
struct Block {
    std::string id;                  ///< block id (chunk id pre-assembly)
    std::string chunk_text;          ///< chunk text (UTF-8) — hash + empty/oversized checks
    std::vector<float> embedding;    ///< upstream embedding (D3 reuse) — semantic dedup + NaN/zero check
    nlohmann::json metadata_json =   ///< JSONB metadata; cleaning writes the "cleaning.*" namespace
        nlohmann::json::object();
    uint8_t flags_ext = 0;           ///< BlockFlagsExt byte; cleaning sets bit2 kFlagExtIsAnomalous

    // --- internal marking (not persisted; used between Dedup phases) ---
    bool marked_for_removal = false; ///< set by Dedup, consumed by the erase step
};

/// Cleaning config. NS-overridable: dedup_enabled /
/// dedup_similarity_threshold / anomaly_detection_enabled. Resource-level
/// (global only, not NS-overridable): max_chunk_chars / plugin_timeout_ms.
/// JSON-serializable so ConfigResolver<CleaningConfig> can three-layer merge
/// it (global ← ns.cleaning_config ← request).
struct CleaningConfig {
    bool dedup_enabled = true;
    double dedup_similarity_threshold = 0.95;   // 0.0-1.0
    bool anomaly_detection_enabled = true;
    int max_chunk_chars = 10000;                // Oversized-chunk threshold (resource level)
    int plugin_timeout_ms = 3000;               // Per-plugin timeout (resource level)
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(CleaningConfig, dedup_enabled,
                                   dedup_similarity_threshold,
                                   anomaly_detection_enabled, max_chunk_chars,
                                   plugin_timeout_ms)

/// Anomaly reason enum (5 categories).
enum class AnomalyReason : uint8_t {
    EMPTY_CHUNK = 1,         // chunk_text.length() == 0
    OVERSIZED_CHUNK = 2,     // chunk_text.length() > max_chunk_chars
    PARSE_FAILED = 3,        // upstream meta.parse_status=failed / meta.parse_failed_page
    DUPLICATE_BLOCK_ID = 4,  // block_id already exists (fallback)
    INVALID_EMBEDDING = 5,   // embedding contains NaN / all zeros
};

/// The §7.bis metric label / cleaning.anomaly_reason token for a reason
/// ("empty" / "oversized" / "parse_failed" / "duplicate_id" / "invalid_embedding").
const char* ToString(AnomalyReason reason);

struct DedupResult {
    int removed_count = 0;
    int kept_count = 0;
    std::vector<std::string> removed_block_ids;  ///< Original IDs of the removed Blocks
    int hash_dedup_count = 0;                     ///< Exact-dedup hit count
    int semantic_dedup_count = 0;                 ///< Semantic-dedup hit count
};

struct AnomalyResult {
    int total_marked = 0;
    std::map<std::string, int> by_reason;         ///< {"empty": 5, "oversized": 2, ...}
};

/// The A-class (data-completeness) summary surfaced to the API caller:
/// the 4 flat fields, NOT the deleted cleaning_applied wrapper.
/// B2 internal stats (hash/semantic split, by_reason, plugin flags) stay in the
/// operation_log, never in the query response.
struct CleaningResult {
    int chunks_input = 0;             ///< Number of input chunks
    int chunks_indexed = 0;           ///< Number of chunks actually indexed (input − dedup_removed)
    int chunks_skipped_dedup = 0;     ///< Skipped due to dedup
    int chunks_marked_anomalous = 0;  ///< Marked anomalous but still indexed (affects score)

    nlohmann::json ToMetaJson() const {
        return nlohmann::json{{"chunks_input", chunks_input},
                              {"chunks_indexed", chunks_indexed},
                              {"chunks_skipped_dedup", chunks_skipped_dedup},
                              {"chunks_marked_anomalous", chunks_marked_anomalous}};
    }
};

}  // namespace cortrix::spc
