#include "cortrix/query/rag_fusion.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "cortrix/query/rag_fusion_error.h"
#include "cortrix/query/rag_fusion_metrics.h"

namespace cortrix::query {

namespace {

// Anchored conservative outer-fusion policy (v1.0.7).
//
// `per_variant_results[0]` is the original query and should remain the primary
// ranking signal. Its top head is anchored as a strong-hit guardrail. LLM
// variants are additive weak evidence: they can promote a candidate that is
// repeatedly found by variants, but they reorder only the non-anchored tail.
//
// v1.0.13 (§4.3.bis.5): the three policy constants moved into RagFusionConfig
// (fusion_original_weight / fusion_variant_weight / fusion_anchor_max) with
// these values as byte-identical defaults — the fixed policy proved
// scale-sensitive (FiQA-5000: -10.8pp recall@10 vs listwise on identical pools).

size_t DynamicOriginalAnchorCount(
    const std::vector<retrieval::ScoredResult>& original_results,
    int anchor_max) {
    if (anchor_max <= 0) return 0;
    const size_t cap = static_cast<size_t>(anchor_max);
    if (original_results.empty()) return 0;
    if (original_results.size() == 1) return 1;

    // Scores arrive from the original query's already-fused retrieval list. They
    // are not cross-query calibrated, so use only head-shape/coherence: a coherent
    // original head deserves the full top-3 guardrail; a steep drop after rank1/2
    // means the tail is weak enough for variant evidence to compete earlier.
    const float top = std::max(0.0f, original_results[0].score);
    if (top <= 0.0f) return 1;
    const float second = std::max(0.0f, original_results[1].score);
    const float third = original_results.size() > 2
                            ? std::max(0.0f, original_results[2].score)
                            : second;
    if (third >= top * 0.45f) {
        return std::min(cap, original_results.size());
    }
    if (second >= top * 0.35f) {
        return std::min(std::min(static_cast<size_t>(2), cap),
                        original_results.size());
    }
    return 1;
}

// Map a failed-Status (carrying a CX_ERR_RAG_FUSION_* prefix from
// RagFusionStatus, and sometimes a nested generic CX_LLM_* token from the shared
// LLM client) back to a human "degrade_reason" token for the ExplainState.
std::string DegradeReasonToken(const Status& status) {
    const std::string& m = status.message();
    auto has = [&](const char* k) { return m.find(k) != std::string::npos; };
    // Prefer generic transport/client diagnostics when present. Otherwise a
    // transport/auth/HTTP failure wrapped as CX_ERR_RAG_FUSION_LLM_TIMEOUT would
    // be indistinguishable from a true deadline in benchmark explain output.
    if (has("CX_LLM_TRANSPORT")) return "llm_transport";
    if (has("CX_LLM_RATE_LIMIT")) return "quota_exceeded";
    if (has("CX_LLM_BAD_BODY")) return "invalid_response";
    if (has("CX_LLM_HTTP")) return "llm_http";
    if (has("LLM_TIMEOUT")) return "llm_timeout";
    if (has("LLM_CIRCUIT_OPEN")) return "circuit_open";
    if (has("LLM_QUOTA")) return "quota_exceeded";
    if (has("INVALID_RESPONSE")) return "invalid_response";
    return "llm_error";
}

std::string SafeDegradeDetail(const Status& status) {
    const std::string& m = status.message();
    const std::string needle = "http_status=";
    std::string detail;
    const auto http_pos = m.find(needle);
    if (http_pos != std::string::npos) {
        std::size_t end = http_pos + needle.size();
        while (end < m.size() && std::isdigit(static_cast<unsigned char>(m[end]))) {
            ++end;
        }
        if (end > http_pos + needle.size()) {
            detail = m.substr(http_pos, end - http_pos);
        }
    }
    if (!detail.empty()) {
        return detail;
    }
    auto has = [&](const char* k) { return m.find(k) != std::string::npos; };
    if (has("CX_LLM_TRANSPORT")) return "transport_failure";
    if (has("CX_LLM_RATE_LIMIT")) return "rate_limited";
    if (has("CX_LLM_BAD_BODY")) return "bad_response_body";
    if (has("ETIMEDOUT") || has("timed out") || has("deadline")) {
        return "deadline_exceeded";
    }
    return "";
}

}  // namespace

RagFusion::RagFusion(std::shared_ptr<QueryVariantGenerator> generator,
                     std::shared_ptr<RRFFusion> rrf)
    : generator_(std::move(generator)), rrf_(std::move(rrf)) {}

void RagFusion::MarkSkippedByActivationPolicy(const std::string& query,
                                              const std::string& reason) {
    explain_ = ExplainState{};
    explain_.active = false;
    explain_.reason = reason;
    explain_.variant_count = 1;
    explain_.variants_used = {query};
}

Result<std::vector<std::string>> RagFusion::ExpandQueries(
    const std::string& query,
    const RagFusionConfig& ns_config,
    const observability::TraceContext* trace_ctx,
    const query::QueryContext* qctx) {
    auto& metrics = RagFusionMetrics::Instance();

    // Reset the explain state for this call; the original query is always present.
    explain_ = ExplainState{};
    explain_.variants_used.push_back(query);
    explain_.variant_count = 1;

    // Deferred: routing-skip is interface-reserved — the frozen
    // QueryContext has no `routing_path` field (the router adds it). Once it
    // lands, this becomes:
    //   if (qctx && QueryComplexityClassifier::ShouldSkipF36(*qctx)) {
    //       explain_.reason = "skipped_by_F39_routing"; ... return {query};
    //   }
    // Until then qctx is accepted + threaded but does not alter behavior.
    (void)qctx;
    (void)trace_ctx;  // §9: TraceContext interface-reserved in V1.0 OSS (always null)

    // topic 3: NS disabled → no LLM call, single query (§4.3).
    if (!ns_config.enabled) {
        explain_.active = false;
        explain_.reason = "ns_disabled";
        metrics.RecordInvocation(RagFusionMetrics::Result::kDisabled);
        return std::vector<std::string>{query};
    }

    // Generate variants via the LLM (§4.2). Time the call for the explain latency.
    const auto t0 = std::chrono::steady_clock::now();
    Result<QueryVariants> gen = generator_->Generate(
        query, ns_config, ns_config.locale, trace_ctx);
    const auto t1 = std::chrono::steady_clock::now();
    const int latency_ms = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count());
    explain_.llm_latency_ms = latency_ms;

    if (!gen.ok()) {
        // topic 4: LLM failure → degrade to single query. The CX_ERR identity is
        // carried in the Status; the caller (QueryPipeline) emits the C-class
        // CX_WARN_RAG_FUSION_DEGRADED warning (§5.3).
        explain_.active = true;
        explain_.degraded = true;
        explain_.degrade_reason = DegradeReasonToken(gen.status());
        explain_.degrade_detail = SafeDegradeDetail(gen.status());
        explain_.reason = "active";
        explain_.variant_count = 1;  // single query
        metrics.RecordInvocation(RagFusionMetrics::Result::kDegraded);
        // RecordDegraded(reason) already emitted inside the generator.
        return gen.status();  // Result<vector<string>> failure carries the CX_ERR
    }

    // Success: all_queries = [original] + variants (§4.5 invariant — original first).
    const QueryVariants& qv = gen.value();
    std::vector<std::string> all_queries;
    all_queries.reserve(qv.variants.size() + 1);
    all_queries.push_back(query);
    for (const std::string& v : qv.variants) all_queries.push_back(v);

    explain_.active = true;
    explain_.reason = "active";
    explain_.degraded = false;
    explain_.variants_used = all_queries;
    explain_.variant_count = static_cast<int>(all_queries.size());

    metrics.RecordInvocation(RagFusionMetrics::Result::kSuccess);
    // §8 variant_count histogram: attribute the produced variants across the
    // configured strategies (round-robin, since the LLM returns a flat list).
    if (!ns_config.variant_strategies.empty()) {
        std::unordered_map<int, int> per_strategy;
        for (size_t i = 0; i < qv.variants.size(); ++i) {
            const VariantStrategy s =
                ns_config.variant_strategies[i % ns_config.variant_strategies.size()];
            per_strategy[static_cast<int>(s)]++;
        }
        for (VariantStrategy s : ns_config.variant_strategies) {
            metrics.ObserveVariantCount(s, per_strategy[static_cast<int>(s)]);
        }
    }
    if (latency_ms > 0) {
        metrics.ObserveLlmLatency(
            qv.llm_model_used.empty() ? "unknown" : qv.llm_model_used, latency_ms);
    }

    return all_queries;
}

Result<std::vector<retrieval::ScoredResult>> RagFusion::FuseResults(
    const std::vector<std::vector<retrieval::ScoredResult>>& per_variant_results,
    int rrf_k,
    const observability::TraceContext* ctx) {
    // Legacy rrf_k-only entry: delegate with the default-constructed policy
    // (fusion_original_weight 1.7 / fusion_variant_weight 0.5 / anchor 3 —
    // byte-identical to the pre-v1.0.13 constants).
    RagFusionConfig cfg;
    cfg.rrf_k = rrf_k;
    return FuseResults(per_variant_results, cfg, ctx);
}

Result<std::vector<retrieval::ScoredResult>> RagFusion::FuseResults(
    const std::vector<std::vector<retrieval::ScoredResult>>& per_variant_results,
    const RagFusionConfig& cfg,
    const observability::TraceContext* ctx) {
    (void)ctx;  // §9: interface-reserved
    int rrf_k = cfg.rrf_k;
    if (rrf_k <= 0) rrf_k = 60;  // RRF formula needs k > 0 (sentinel ARCH default)
    const double original_weight = static_cast<double>(cfg.fusion_original_weight);
    const double variant_weight = static_cast<double>(cfg.fusion_variant_weight);

    const auto t0 = std::chrono::steady_clock::now();

    // Global second-pass RRF over the ChildId keyspace (topic 2 B). v1.0.7 makes
    // this an anchored conservative weighted RRF: list 0 is the original query and
    // carries the primary weight; its top head is emitted first as a strong-hit
    // guardrail; LLM-variant lists carry weaker additive evidence for the
    // remaining order.
    //
    // contribution(query_index, rank) = role_weight / (k + rank)
    //
    // Sum per ChildId (dedup keep-sum); keep the max raw score seen for
    // tie-context / downstream display. This is the outer fusion over
    // retrieval::ScoredResult, distinct from the inner per-NS RRFFusion.
    struct Acc {
        double rrf = 0.0;
        float  best_raw = 0.0f;
        size_t first_seen = 0;  // stable-sort tiebreak (insertion order)
    };
    std::unordered_map<retrieval::ChildId, Acc> acc;
    acc.reserve(per_variant_results.size() * 32);
    size_t order = 0;

    for (size_t query_index = 0; query_index < per_variant_results.size(); ++query_index) {
        const auto& variant_list = per_variant_results[query_index];
        const double role_weight = query_index == 0 ? original_weight : variant_weight;
        for (size_t r = 0; r < variant_list.size(); ++r) {
            const retrieval::ScoredResult& sr = variant_list[r];
            Acc& a = acc[sr.child_id];
            if (a.rrf == 0.0 && a.best_raw == 0.0f) a.first_seen = order++;
            a.rrf += role_weight /
                     (static_cast<double>(rrf_k) + static_cast<double>(r + 1));
            a.best_raw = std::max(a.best_raw, sr.score);
        }
    }

    std::vector<std::pair<retrieval::ChildId, Acc>> rows(acc.begin(), acc.end());
    std::sort(rows.begin(), rows.end(),
              [](const auto& x, const auto& y) {
                  if (x.second.rrf != y.second.rrf) return x.second.rrf > y.second.rrf;
                  return x.second.first_seen < y.second.first_seen;  // stable
              });

    std::vector<retrieval::ScoredResult> fused;
    fused.reserve(acc.size());
    std::unordered_set<retrieval::ChildId> emitted;
    emitted.reserve(std::min(acc.size(),
                             static_cast<size_t>(std::max(cfg.fusion_anchor_max, 0))) +
                    8);

    if (!per_variant_results.empty()) {
        const auto& original_results = per_variant_results.front();
        const size_t anchor_limit =
            DynamicOriginalAnchorCount(original_results, cfg.fusion_anchor_max);
        size_t anchored_count = 0;
        for (const auto& sr : original_results) {
            if (anchored_count >= anchor_limit) break;
            const retrieval::ChildId& child_id = sr.child_id;
            if (!emitted.insert(child_id).second) continue;
            auto found = acc.find(child_id);
            if (found == acc.end()) continue;
            retrieval::ScoredResult out;
            out.child_id = child_id;
            out.score = static_cast<float>(found->second.rrf);
            fused.push_back(std::move(out));
            ++anchored_count;
        }
    }

    for (auto& row : rows) {
        if (emitted.find(row.first) != emitted.end()) continue;
        retrieval::ScoredResult out;
        out.child_id = row.first;
        out.score = static_cast<float>(row.second.rrf);  // the RRF score
        fused.push_back(std::move(out));
    }

    const auto t1 = std::chrono::steady_clock::now();
    RagFusionMetrics::Instance().ObserveRrfFusionDuration(static_cast<int>(
        std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count()));

    return fused;
}

}  // namespace cortrix::query
