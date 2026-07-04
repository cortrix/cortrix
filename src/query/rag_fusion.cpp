#include "cortrix/query/rag_fusion.h"

#include <algorithm>
#include <chrono>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "cortrix/query/rag_fusion_error.h"
#include "cortrix/query/rag_fusion_metrics.h"

namespace cortrix::query {

namespace {

// F36 v1.0.7 anchored conservative outer-fusion policy.
//
// `per_variant_results[0]` is the original query and should remain the primary
// ranking signal. Its top-3 head is anchored as a strong-hit guardrail. LLM
// variants are additive weak evidence: they can promote a candidate that is
// repeatedly found by variants, but they reorder only the non-anchored tail.
constexpr double kOriginalQueryRrfWeight = 1.7;
constexpr double kVariantQueryRrfWeight = 0.5;
constexpr size_t kOriginalQueryAnchorCount = 3;

double QueryRoleWeight(size_t query_index) {
    return query_index == 0 ? kOriginalQueryRrfWeight : kVariantQueryRrfWeight;
}

// Map a failed-Status (carrying a CX_ERR_RAG_FUSION_* prefix from
// RagFusionStatus) back to a human "degrade_reason" token for the ExplainState
// (§4.3) — "llm_timeout" / "circuit_open" / "quota_exceeded" / "invalid_response".
std::string DegradeReasonToken(const Status& status) {
    const std::string& m = status.message();
    auto has = [&](const char* k) { return m.find(k) != std::string::npos; };
    if (has("LLM_TIMEOUT"))      return "llm_timeout";
    if (has("LLM_CIRCUIT_OPEN")) return "circuit_open";
    if (has("LLM_QUOTA"))        return "quota_exceeded";
    if (has("INVALID_RESPONSE")) return "invalid_response";
    return "llm_error";
}

}  // namespace

RagFusion::RagFusion(std::shared_ptr<QueryVariantGenerator> generator,
                     std::shared_ptr<RRFFusion> rrf)
    : generator_(std::move(generator)), rrf_(std::move(rrf)) {}

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

    // 🔵 D3.5 deferred (§4.8.2): F39 routing-skip is interface-reserved — the frozen
    // QueryContext has no `routing_path` field (F39 adds it, same Wave). Once F39
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
    (void)ctx;  // §9: interface-reserved
    if (rrf_k <= 0) rrf_k = 60;  // RRF formula needs k > 0 (sentinel ARCH default)

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
    // tie-context / downstream display. This is the outer F36 fusion over
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
        const double role_weight = QueryRoleWeight(query_index);
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
    emitted.reserve(std::min(acc.size(), kOriginalQueryAnchorCount) + 8);

    if (!per_variant_results.empty()) {
        const auto& original_results = per_variant_results.front();
        size_t anchored_count = 0;
        for (const auto& sr : original_results) {
            if (anchored_count >= kOriginalQueryAnchorCount) break;
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
