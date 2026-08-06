#include "cortrix/reranker/reranker_config_resolver.h"

#include <algorithm>

namespace cortrix::reranker {

int EffectiveRerankerConfig::ComputeTopN(int top_k) const {
    if (top_k <= 0) return 0;
    // E2: top_N = min(top_k × multiplier, max_candidates).
    const int mult = std::max(1, candidate_multiplier);
    long top_n = std::min<long>(static_cast<long>(top_k) * mult, max_candidates);
    // Safety floor: never below top_k (a too-small max_candidates must not make
    // the reranker return fewer than the requested results).
    return static_cast<int>(std::max<long>(top_n, top_k));
}

EffectiveRerankerConfig RerankerConfigResolver::Resolve(
    const NsRerankerConfig& ns, const RerankerRequestParams& request) const {
    EffectiveRerankerConfig eff;

    // enabled: request > NS > global. The request `rerank` boolean, when
    // present, wins over everything (the example: NS enabled=false but
    // request rerank=true → enabled).
    if (request.rerank.has_value()) {
        eff.enabled = *request.rerank;
    } else if (ns.enabled.has_value()) {
        eff.enabled = *ns.enabled;
    } else {
        eff.enabled = global_enabled_;
    }

    // candidate_multiplier / max_candidates: NS > global (no request layer).
    eff.candidate_multiplier =
        ns.candidate_multiplier.value_or(global_.candidate_multiplier);
    eff.max_candidates = ns.max_candidates.value_or(global_.max_candidates);

    return eff;
}

Result<EffectiveRerankerConfig> RerankerConfigResolver::Resolve(
    const std::string& ns_json, const RerankerRequestParams& request) const {
    Result<NsRerankerConfig> ns = NsRerankerConfig::Parse(ns_json);
    if (!ns.ok()) return ns.status();
    return Resolve(ns.value(), request);
}

}  // namespace cortrix::reranker
