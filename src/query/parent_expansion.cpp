#include "cortrix/query/parent_expansion.h"

#include <algorithm>
#include <string>
#include <utility>

namespace cortrix::query {

std::vector<ParentExpansionTarget> SelectParentExpansionTargets(const CrossNsResponse& resp,
                                                                int expand_top_k) {
    std::vector<ParentExpansionTarget> targets;
    if (expand_top_k <= 0) return targets;

    const std::size_t window = static_cast<std::size_t>(expand_top_k);
    targets.reserve(window);
    for (std::size_t i = 0; i < resp.results.size() && targets.size() < window; ++i) {
        const retrieval::ResultItem& item = resp.results[i];
        if (item.parent_id.empty()) continue;  // nothing to look up; does not use depth
        ParentExpansionTarget t;
        t.namespace_id = item.namespace_id;
        t.parent_id = item.parent_id;
        t.result_index = i;
        targets.push_back(std::move(t));
    }
    return targets;
}

ParentTextByKey FetchParentsForTargets(const std::vector<ParentExpansionTarget>& targets,
                                       const NamespaceParentFetch& fetch) {
    ParentTextByKey out;
    if (!fetch) return out;

    // First-seen namespace order, and within a namespace first-seen id order, so the
    // lookups a test observes follow the response order rather than a map's ordering.
    std::vector<std::string> ns_order;
    std::map<std::string, std::vector<std::string>> ids_by_ns;
    for (const auto& t : targets) {
        auto it = ids_by_ns.find(t.namespace_id);
        if (it == ids_by_ns.end()) {
            ns_order.push_back(t.namespace_id);
            it = ids_by_ns.emplace(t.namespace_id, std::vector<std::string>{}).first;
        }
        std::vector<std::string>& ids = it->second;
        if (std::find(ids.begin(), ids.end(), t.parent_id) == ids.end()) {
            ids.push_back(t.parent_id);  // DISTINCT per namespace — one lookup each
        }
    }

    for (const std::string& ns : ns_order) {
        for (CortrixParent& row : fetch(ns, ids_by_ns[ns])) {
            if (row.parent_id.empty()) continue;  // unkeyable row
            out.emplace(std::make_pair(ns, row.parent_id), std::move(row));
        }
    }
    return out;
}

int ApplyParentExpansion(CrossNsResponse& resp,
                         const std::vector<ParentExpansionTarget>& targets,
                         const ParentTextByKey& parents,
                         int max_bytes) {
    long long used_bytes = 0;
    int expanded = 0;

    for (const auto& t : targets) {
        if (t.result_index >= resp.results.size()) continue;  // stale target; ignore
        auto it = parents.find(std::make_pair(t.namespace_id, t.parent_id));
        if (it == parents.end()) continue;  // dangling link — degrade, do not stop
        const CortrixParent& parent = it->second;

        // Charged PER RESULT, not per distinct parent: two results sharing a parent
        // each carry their own copy of the text on the wire, so the response pays
        // twice even though the lookup happened once.
        const long long cost = static_cast<long long>(parent.parent_text.size());
        if (max_bytes > 0 && used_bytes + cost > static_cast<long long>(max_bytes)) {
            break;  // prefix semantics — see the header
        }
        used_bytes += cost;

        retrieval::ResultItem& item = resp.results[t.result_index];
        item.parent_content = parent.parent_text;
        // Where the expanded span sits in the document. Written only for results that
        // actually received text, so these keys never advertise an expansion that is
        // not in the payload.
        item.metadata["parent_token_count"] = std::to_string(parent.token_count);
        item.metadata["parent_byte_start"] = std::to_string(parent.byte_offset_start);
        item.metadata["parent_byte_end"] = std::to_string(parent.byte_offset_end);
        ++expanded;
    }
    return expanded;
}

}  // namespace cortrix::query
