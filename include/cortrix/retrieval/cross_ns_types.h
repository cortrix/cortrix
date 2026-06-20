#pragma once
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cortrix/retrieval/types.h"  // ChildId / ParentId / RankedChunk (F02 frozen)

namespace cortrix::retrieval {

/// ResultItem + NamespaceQueryResult — RETRIEVAL_TYPES_SPEC §1-bis (P0-V5-05).
///
/// 🔑 Clarification (Lead): the SoT §1-bis *specifies* these two types in
/// `cortrix::retrieval`, but the frozen F02 `retrieval/types.h` only shipped
/// ScoredResult + RankedChunk ("defined in 0 places across the whole repo per the SoT" — they were never coded). F04 is
/// their first consumer, so F04 introduces them HERE — in the SoT-mandated
/// `cortrix::retrieval` namespace — additively, without editing the frozen header.
/// If a later feature also needs them, this is the canonical home (no per-feature
/// re-definition; RETRIEVAL_TYPES_SPEC §3.3 evolution rule).

/// F04 single-NS query result — the Map-stage output of ScatterGather
/// (RETRIEVAL_TYPES_SPEC §1-bis). A per-NS failure is carried in-band via a
/// non-empty `error_code` (+ category / retryable / retry_after_ms), so
/// ScatterGather folds it into meta.namespaces_failed[] without losing the other
/// NS's results (topic 2.4 partial success).
struct NamespaceQueryResult {
    std::string namespace_id;          ///< the NS this result is for
    std::vector<RankedChunk> chunks;   ///< RankedChunks returned by the NS
    int latency_ms = 0;                ///< per-NS query latency
    std::string error_code;            ///< empty = success; else CX_ERR_*
    std::string error_category;        ///< auth/quota/transient/permanent/timeout
    bool retryable = false;
    int retry_after_ms = 0;
    /// GEN-Agent #5 structured_data the failure body MUST carry for its code
    /// (cross_ns_error.h RequiredStructuredDataKeys). e.g. kIndexCorrupt needs
    /// {index_state}; "namespace" is injected at serialization. Empty on success.
    nlohmann::json structured_data = nlohmann::json::object();
};

/// F04 client-facing top-level item — fuses RankedChunk + content_hash + dedupe
/// provenance (RETRIEVAL_TYPES_SPEC §1-bis). `child_id`/`parent_id` are the ULID
/// string primary identifiers (V5 ruling #6A — no block_id:int). `content_hash` is
/// the "sha256:<32-hex>" representation (§6).
struct ResultItem {
    ChildId     child_id;              ///< ★ primary identifier (V5 ruling #6A)
    ParentId    parent_id;             ///< ★ primary identifier (V5 ruling #6A)
    std::string namespace_id;          ///< primary NS (highest rerank_score source)
    std::string content;               ///< ≅ RankedChunk.chunk_text
    std::string parent_content;        ///< ≅ RankedChunk.parent_text
    float       score = 0.0f;          ///< fused final_score (F07-5 SemanticScorer output)
    float       rerank_score = 0.0f;   ///< primary NS cross-encoder score
    std::string content_hash;          ///< "sha256:<32-hex>" (§6 unified representation)
    std::map<std::string, std::string> metadata;  ///< primary NS full metadata
};

/// RankedChunk → ResultItem (RETRIEVAL_TYPES_SPEC §1-bis ToResultItem). `ns_id` is
/// the source NS; `content_hash_str` is the "sha256:..." key (from the F09 header
/// at D3.5, or content-derived standalone).
ResultItem ToResultItem(const RankedChunk& rc,
                        const std::string& ns_id,
                        const std::string& content_hash_str);

}  // namespace cortrix::retrieval
