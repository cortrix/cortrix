#include "cortrix/query/query_context_explain.h"

namespace cortrix::query {

nlohmann::json ToExplainJson(const QueryContext& ctx) {
    // Key order follows QUERY_CONTEXT_SPEC §5.1 (A class, then B class, then C
    // class). nlohmann::json object preserves insertion order.
    nlohmann::json j;

    // --- A class (always exposed in the real endpoint) ---
    j["query"] = ctx.query;
    j["ns_id"] = ctx.ns_id;
    j["multi_turn_context_warning"] = ctx.multi_turn_context_warning;
    j["web_fallback_triggered"] = ctx.web_fallback_triggered;

    // --- B class (only via ?explain=true) ---
    j["routing_path"] = ctx.routing_path;
    j["complexity_score"] = ctx.complexity_score;
    j["routing_decision_source"] = ctx.routing_decision_source;
    j["chat_path_triggered"] = ctx.chat_path_triggered;
    j["crag_verdict"] = ctx.crag_verdict;
    j["crag_score"] = ctx.crag_score;
    j["ambiguous_action_taken"] = ctx.ambiguous_action_taken;

    // --- C class (anomaly debug; only when the capability ran + failed / explain) ---
    j["f37_signals"] = ctx.f37_signals;        // map<string,float> → JSON object
    j["routing_misclassified"] = ctx.routing_misclassified;

    return j;
}

nlohmann::json BuildExplainNode(const QueryContext& ctx, bool include_debug) {
    // Key order follows QUERY_CONTEXT_SPEC §5.1 (A class, then B class, then C
    // class). nlohmann::json object preserves insertion order.
    nlohmann::json j;

    // --- A class (data integrity — always present) ---
    j["query"] = ctx.query;
    j["ns_id"] = ctx.ns_id;
    j["multi_turn_context_warning"] = ctx.multi_turn_context_warning;
    j["web_fallback_triggered"] = ctx.web_fallback_triggered;

    // --- B class (LLM-dependent path — present because explain was requested) ---
    j["routing_path"] = ctx.routing_path;
    j["complexity_score"] = ctx.complexity_score;
    j["routing_decision_source"] = ctx.routing_decision_source;
    j["chat_path_triggered"] = ctx.chat_path_triggered;
    j["crag_verdict"] = ctx.crag_verdict;
    j["crag_score"] = ctx.crag_score;
    j["ambiguous_action_taken"] = ctx.ambiguous_action_taken;

    // --- C class (anomaly debug — only when a capability ran AND failed) ---
    // SPEC §3 C-class rule: do not surface these unless explicitly triggered, so a
    // default-valued signal is never mistaken for a real one.
    if (include_debug) {
        j["f37_signals"] = ctx.f37_signals;        // map<string,float> → JSON object
        j["routing_misclassified"] = ctx.routing_misclassified;
    }

    return j;
}

}  // namespace cortrix::query
