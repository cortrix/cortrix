#include "cortrix/query/cross_ns_response.h"

#include <utility>

namespace cortrix::query {

namespace {

// One client-facing result item (§2.5 schema). child_id/parent_id are ULID strings
// (V5 decision #6A — never an int block_id, JS-precision-safe). content_hash is the
// "sha256:..." string. metadata is a flat string→string object.
nlohmann::json ResultItemToJson(const retrieval::ResultItem& it) {
    nlohmann::json j;
    j["child_id"] = it.child_id;
    j["parent_id"] = it.parent_id;
    j["content"] = it.content;
    j["parent_content"] = it.parent_content;
    j["score"] = it.score;
    j["rerank_score"] = it.rerank_score;
    j["namespace"] = it.namespace_id;
    j["content_hash"] = it.content_hash;
    j["metadata"] = it.metadata;  // std::map<string,string> → JSON object
    if (it.score_signals.HasAny()) {
        nlohmann::json signals = nlohmann::json::object();
        if (it.score_signals.enriched_score.has_value()) {
            signals["enriched_score"] = *it.score_signals.enriched_score;
        }
        if (it.score_signals.semantic_score.has_value()) {
            signals["semantic_score"] = *it.score_signals.semantic_score;
        }
        j["score_signals"] = std::move(signals);
    }
    return j;
}

// One meta.namespaces_failed[] entry (§2.5 + GEN-Agent 4 fields).
nlohmann::json FailureToJson(const NamespaceFailure& f) {
    nlohmann::json j;
    j["namespace"] = f.namespace_id;
    j["error_code"] = f.error_code;
    j["retryable"] = f.retryable;
    j["category"] = f.category;
    j["retry_after_ms"] = f.retry_after_ms.has_value()
                              ? nlohmann::json(*f.retry_after_ms)
                              : nlohmann::json(nullptr);
    j["message"] = f.message;
    // GEN-Agent #5: every failure body carries the structured_data its code
    // requires (cross_ns_error.h RequiredStructuredDataKeys). "namespace" is
    // always required and always known here, so inject it; overlay any code-
    // specific keys the producer set (e.g. kIndexCorrupt's index_state).
    nlohmann::json sd = nlohmann::json::object();
    sd["namespace"] = f.namespace_id;
    if (f.structured_data.is_object()) {
        for (auto it = f.structured_data.begin(); it != f.structured_data.end(); ++it) {
            sd[it.key()] = it.value();
        }
    }
    j["structured_data"] = std::move(sd);
    return j;
}

// One meta.deduplicated_chunks[] entry (§2.5 / §3.3 B-simplified).
nlohmann::json DedupToJson(const DeduplicatedChunkInfo& d) {
    nlohmann::json j;
    j["content_hash"] = d.content_hash;
    j["primary_namespace"] = d.primary_namespace;
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& src : d.namespaces) {
        arr.push_back({{"namespace", src.namespace_id},
                       {"score", src.score},
                       {"rerank_score", src.rerank_score}});
    }
    j["namespaces"] = std::move(arr);
    return j;
}

}  // namespace

nlohmann::json CrossNsMeta::ToJson() const {
    nlohmann::json j;
    j["namespaces_queried"] = namespaces_queried;
    j["namespaces_succeeded"] = namespaces_succeeded;

    nlohmann::json failed = nlohmann::json::array();
    for (const auto& f : namespaces_failed) failed.push_back(FailureToJson(f));
    j["namespaces_failed"] = std::move(failed);

    j["coverage_ratio"] = coverage_ratio;
    j["latency_ms"] = latency_ms;

    nlohmann::json dedup = nlohmann::json::array();
    for (const auto& d : deduplicated_chunks) dedup.push_back(DedupToJson(d));
    j["deduplicated_chunks"] = std::move(dedup);

    j["deduplicated_chunks_count"] = deduplicated_chunks_count;
    j["warnings"] = warnings;
    return j;
}

nlohmann::json CrossNsResponse::ToJson() const {
    nlohmann::json j;
    nlohmann::json results_arr = nlohmann::json::array();
    for (const auto& it : results) results_arr.push_back(ResultItemToJson(it));
    j["results"] = std::move(results_arr);
    j["meta"] = meta.ToJson();
    if (error.has_value()) {
        j["error"] = agent_friendly::ToJson(*error);
    }
    return j;
}

}  // namespace cortrix::query
