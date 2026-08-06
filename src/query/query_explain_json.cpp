#include "cortrix/query/query_explain_json.h"

namespace cortrix::query {

using nlohmann::json;

json BuildRagFusionExplain(const RagFusion::ExplainState& es) {
    json rf = {
        {"active", es.active},
        {"reason", es.reason},
        {"variant_count", es.variant_count},
        {"degraded", es.degraded},
    };
    if (!es.variants_used.empty()) {
        rf["variants_used"] = es.variants_used;
    }
    if (!es.degrade_reason.empty()) {
        rf["degrade_reason"] = es.degrade_reason;
    }
    if (!es.degrade_detail.empty()) {
        rf["degrade_detail"] = es.degrade_detail;
    }
    if (es.llm_latency_ms.has_value()) {
        rf["llm_latency_ms"] = *es.llm_latency_ms;
    }
    return rf;
}

json BuildListwiseRerankExplain(const LlmRerankStage::ExplainState& es) {
    json lr = {
        {"active", es.active},
        {"reason", es.reason},
        {"top_n_effective", es.top_n_effective},
        {"order_changed", es.order_changed},
        {"degraded", es.degraded},
    };
    if (!es.model_used.empty()) {
        lr["model"] = es.model_used;
    }
    if (!es.degrade_reason.empty()) {
        lr["degrade_reason"] = es.degrade_reason;
    }
    if (!es.degrade_detail.empty()) {
        lr["degrade_detail"] = es.degrade_detail;
    }
    if (es.active) {
        lr["llm_latency_ms"] = es.llm_latency_ms;
        lr["llm_calls"] = es.llm_calls;
        lr["votes_ok"] = es.votes_ok;
    }
    return lr;
}

}  // namespace cortrix::query
