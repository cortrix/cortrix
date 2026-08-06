#include "cortrix/chunker/chunker_response_meta.h"

#include "cortrix/chunker/chunker_agent_error.h"

namespace cortrix::chunker {

nlohmann::json BuildStatsMeta(const ChunkerStats& stats) {
    // stats shape (A-class: document-parse completeness).
    nlohmann::json s;
    s["total_pages"] = stats.total_pages;
    s["succeeded_pages"] = stats.succeeded_pages;
    s["failed_pages"] = stats.failed_pages;
    s["total_parents"] = stats.total_parents;
    s["total_children"] = stats.total_children;
    s["coverage_ratio"] = stats.CoverageRatio();
    return s;
}

nlohmann::json BuildWarningsMeta(const ChunkerStats& stats, const std::string& doc_id,
                                 int parent_count, int threshold) {
    nlohmann::json warnings = nlohmann::json::array();
    if (stats.fallback_to_flat) {
        // CX_WARN_CHUNK_FALLBACK structured_data = {doc_id, parent_count, threshold}
        //. Non-blocking; surfaced to the client so it knows parent-child was degraded.
        warnings.push_back(MakeChunkWarning(
            ChunkerWarningCode::kFallback,
            {{"doc_id", doc_id}, {"parent_count", parent_count}, {"threshold", threshold}},
            "document exceeded max_parents_per_doc; chunked as flat (no parent-child)"));
    }
    return warnings;
}

nlohmann::json BuildChildrenHitsMetaJson(const std::vector<ParentHitGroup>& groups) {
    // children_hits_per_parent: [{parent_id, hits:[...], primary_child}, ...].
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& e : BuildChildrenHitsMeta(groups)) {
        nlohmann::json o;
        o["parent_id"] = e.parent_id;
        o["hits"] = e.hits;
        o["primary_child"] = e.primary_child;
        arr.push_back(std::move(o));
    }
    return arr;
}

}  // namespace cortrix::chunker
