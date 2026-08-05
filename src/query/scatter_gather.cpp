#include <cstdint>
#include "cortrix/query/scatter_gather.h"

#include <algorithm>
#include <chrono>
#include <future>
#include <unordered_map>
#include <utility>

#include "cortrix/query/authorize_namespaces.h"
#include "cortrix/query/content_hash.h"
#include "cortrix/query/cross_ns_dedup.h"
#include "cortrix/query/cross_ns_error.h"
#include "cortrix/query/scatter_guc.h"
#include "cortrix/query/scatter_metrics.h"
#include "cortrix/query/scatter_plan.h"

namespace cortrix::query {

using retrieval::NamespaceQueryResult;
using retrieval::RankedChunk;
using retrieval::ResultItem;

ScatterGather::ScatterGather(IScatterExecutor* executor,
                             ExecutorEngine* engine,
                             reranker::IReranker* reranker,
                             PermissionService* perm)
    : executor_(executor), engine_(engine), reranker_(reranker), perm_(perm) {}

void ScatterGather::ApplyConfig(const ScatterConfig& cfg) {
    // S4.2: the GUC-resolved global NS cap + dual-layer timeouts. executor.workers /
    // executor.queue_size sized the ExecutorEngine at construction (owned by the
    // caller), so they are not re-applied here.
    SetMaxNamespaces(cfg.max_namespaces_per_query);
    SetTimeouts(cfg.Timeouts());
}

QueryContext ScatterGather::MakeContext(const QueryRequest& request,
                                        const AuthContext& auth,
                                        const TraceContext* trace_ctx) const {
    QueryContext ctx;
    ctx.query = request.query;
    ctx.top_k = request.top_k < 1 ? 1 : request.top_k;
    ctx.rerank = request.rerank;
    ctx.filter = request.filter;
    ctx.enable_vector = request.search_config.enable_vector;
    ctx.enable_bm25 = request.search_config.enable_bm25;
    ctx.enable_sparse = request.search_config.enable_sparse;
    ctx.user_id = auth.user_id;
    ctx.tenant_id = auth.tenant_id;
    if (trace_ctx != nullptr) ctx.trace_id = trace_ctx->trace_id;
    return ctx;
}

CrossNsResponse ScatterGather::Execute(const QueryRequest& request,
                                       const AuthContext& auth,
                                       const QueryContext* qctx,
                                       const TraceContext* trace_ctx) {
    // 1. Authorization (§4.2 5 steps). The effective per-query cap is min(global GUC
    //    cap, AuthContext plan cap) — S4.3 Cloud P01-3 plan coordination (topic 1.5). Throws
    //    CrossNsException on auth / too-many / unauthorized — the handler serializes
    //    GetError() to the §2.6 body.
    const int effective_cap = EffectiveMaxNamespaces(max_namespaces_, auth);
    std::vector<std::string> authorized =
        AuthorizeNamespaces(request.namespaces, auth, perm_, effective_cap);

    // Shared per-request context: caller-supplied or built here.
    QueryContext built;
    const QueryContext* ctx_ptr = qctx;
    if (ctx_ptr == nullptr) {
        built = MakeContext(request, auth, trace_ctx);
        ctx_ptr = &built;
    }
    const QueryContext& ctx = *ctx_ptr;

    // §3.4 request counter: classify the request shape for the `reason` label
    // (wildcard takes precedence over the post-expansion NS count). category here is
    // the success class (kPermanent) — hard-rejected requests threw above and are
    // not counted as served requests.
    const bool is_wildcard =
        request.namespaces.size() == 1 && request.namespaces.front() == "*";
    const ScatterMetrics::Reason reason =
        is_wildcard ? ScatterMetrics::Reason::kWildcard
                    : (authorized.size() == 1 ? ScatterMetrics::Reason::kSingle
                                              : ScatterMetrics::Reason::kMulti);
    ScatterMetrics::Instance().RecordRequest(reason,
                                             agent_friendly::ErrorCategory::kPermanent);

    // 2. Single / multi-NS path split (topic 1.6).
    if (authorized.size() == 1) {
        return ExecuteSingleNs(ctx, authorized.front());
    }
    return ExecuteMultiNs(ctx, authorized);
}

CrossNsResponse ScatterGather::ExecuteSingleNs(const QueryContext& ctx,
                                               const std::string& ns) {
    // Direct path — no ExecutorEngine (topic 1.6). One NS's result, then gather.
    NamespaceQueryResult r = executor_->ExecuteForNamespace(ctx, ns, /*oversample=*/1.0f);
    std::vector<NamespaceQueryResult> results;
    results.push_back(std::move(r));
    return GatherAndRerank(std::move(results), {ns}, ctx.top_k, ctx.rerank,
                           /*scatter_timed_out=*/false);
}

CrossNsResponse ScatterGather::ExecuteMultiNs(const QueryContext& ctx,
                                              const std::vector<std::string>& nss) {
    using clock = std::chrono::steady_clock;
    const auto start = clock::now();
    const auto overall_deadline =
        start + std::chrono::milliseconds(timeouts_.total_timeout_ms);

    // Fan out: one task per NS onto the shared ExecutorEngine (topic 1.2 / 1.3).
    // Each task captures ns by value; the executor is reentrant per NS.
    std::vector<std::future<NamespaceQueryResult>> futures;
    futures.reserve(nss.size());
    for (const std::string& ns : nss) {
        IScatterExecutor* exec = executor_;
        const QueryContext* cptr = &ctx;
        futures.push_back(engine_->Submit<NamespaceQueryResult>(
            [exec, cptr, ns]() { return exec->ExecuteForNamespace(*cptr, ns, 1.0f); }));
    }

    // Dual-layer timeout join (topic 2.5): each NS gets ns_query_timeout_ms, but never
    // waits past the overall total_timeout_ms. A NS not ready by its deadline is
    // recorded as CX_ERR_NS_TIMEOUT; if the overall deadline is what cut it off,
    // the response also carries the top-level CX_ERR_SCATTER_TIMEOUT (partial).
    std::vector<NamespaceQueryResult> results(nss.size());
    bool scatter_timed_out = false;

    for (std::size_t i = 0; i < futures.size(); ++i) {
        const auto ns_deadline =
            clock::now() + std::chrono::milliseconds(timeouts_.ns_query_timeout_ms);
        const auto deadline = std::min(ns_deadline, overall_deadline);

        std::future_status st = futures[i].wait_until(deadline);
        if (st == std::future_status::ready) {
            results[i] = futures[i].get();
        } else {
            // Timed out. Distinguish per-NS vs overall for the right top-level code.
            const bool past_overall = clock::now() >= overall_deadline;
            if (past_overall) scatter_timed_out = true;
            const auto& info = GetCrossNsErrorInfo(CrossNsErrorCode::kNsTimeout);
            NamespaceQueryResult timed;
            timed.namespace_id = nss[i];
            timed.error_code = info.cx_code;  // CX_ERR_NS_TIMEOUT for the per-NS row
            timed.error_category = agent_friendly::ToString(info.category);
            timed.retryable = info.retryable;
            timed.retry_after_ms = info.retry_after_ms.value_or(0);
            timed.latency_ms = static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - start)
                    .count());
            results[i] = std::move(timed);
            // NOTE: the detached task may still finish on the pool; its result is
            // discarded. Cooperative per-NS cancellation = D3.5 (needs pipeline support).
        }
    }

    return GatherAndRerank(std::move(results), nss, ctx.top_k, ctx.rerank,
                           scatter_timed_out);
}

CrossNsResponse ScatterGather::GatherAndRerank(
    std::vector<NamespaceQueryResult> results,
    const std::vector<std::string>& queried_order,
    int top_k,
    bool rerank,
    bool scatter_timed_out) {
    CrossNsResponse resp;
    CrossNsMeta& meta = resp.meta;
    meta.namespaces_queried = queried_order;

    // Collect successful chunks → ResultItems; failures → meta.namespaces_failed[].
    std::vector<ResultItem> items;
    for (auto& r : results) {
        if (r.error_code.empty()) {
            meta.namespaces_succeeded.push_back(r.namespace_id);
            for (auto& rc : r.chunks) {
                // content_hash (F09-4 hook, R3 closed): the authoritative source is
                // the per-Unit blocks.content_hash column (idx_block_hash) — written
                // by block_header.cpp as SHA-256(content_text) first-16-bytes. This
                // ContentHashOfContent(chunk_text) is the SAME computation over the
                // SAME bytes (ChunkRecord.content == blocks.content_text), so it is
                // byte-identical to the stored column with zero extra read IO. The
                // shared cross-NS dedup key is therefore the authoritative hash.
                std::string hash = ContentHashOfContent(rc.chunk_text);
                items.push_back(retrieval::ToResultItem(rc, r.namespace_id, hash));
            }
        } else {
            NamespaceFailure f;
            f.namespace_id = r.namespace_id;
            f.error_code = r.error_code;
            f.retryable = r.retryable;
            f.category = r.error_category;
            if (r.retryable && r.retry_after_ms > 0) f.retry_after_ms = r.retry_after_ms;
            f.message = r.error_code;
            f.structured_data = std::move(r.structured_data);  // GEN-Agent #5 (e.g. index_state)
            meta.namespaces_failed.push_back(std::move(f));
        }
    }

    // Gather (§4.1 step 3): rank by final score, then B-simplified cross-NS
    // dedup (§3.3 — collapses copies of one content_hash to the highest-score
    // primary + records a brief multi-source note in meta), then truncate to
    // top_k. Sort-before-dedup keeps the deduped list in descending final score
    // (each group's primary is its first occurrence in the sorted list).
    std::stable_sort(items.begin(), items.end(),
                     [](const ResultItem& a, const ResultItem& b) {
                         return a.score > b.score;
                     });

    // §3.3 / §4.3: populates meta.deduplicated_chunks[] + deduplicated_chunks_count.
    const int collapsed = DedupeByContentHash(items, meta);
    if (collapsed > 0) {
        ScatterMetrics::Instance().RecordDedupCollisions(
            static_cast<uint64_t>(collapsed));
    }

    const int k = top_k < 1 ? 1 : top_k;
    if (static_cast<int>(items.size()) > k) {
        items.resize(static_cast<std::size_t>(k));
    }
    resp.results = std::move(items);

    // RRF fallback status notice (topic 3.2): when rerank is off, surface the C-class
    // warning so an Agent knows ordering came from RRF, not the cross-encoder.
    if (!rerank) {
        meta.warnings.push_back({{"code", "CX_WARN_RERANK_DISABLED"},
                                 {"reason", "rerank_disabled"},
                                 {"category", "permanent"}});
    }

    // 8-field meta completion (§2.5 / topic 3.5 v1.0.2). deduplicated_chunks[] +
    // deduplicated_chunks_count were filled by DedupeByContentHash above.
    const int queried = static_cast<int>(meta.namespaces_queried.size());
    const int succeeded = static_cast<int>(meta.namespaces_succeeded.size());
    meta.coverage_ratio = queried > 0
                              ? static_cast<float>(succeeded) / static_cast<float>(queried)
                              : 1.0f;

    int total_latency = 0;
    for (const auto& r : results) total_latency = std::max(total_latency, r.latency_ms);
    meta.latency_ms = total_latency;

    // Observability (§3.4): NS-count + latency histograms, and the partial-success
    // counter when any NS failed (a partial-success response always carries a
    // non-empty meta.namespaces_failed[]). Standalone recorder; F24 /metrics = D3.5.
    ScatterMetrics& m = ScatterMetrics::Instance();
    m.ObserveNamespacesPerQuery(queried);
    m.ObserveDuration(queried, total_latency);
    if (!meta.namespaces_failed.empty()) m.RecordPartialSuccess();

    // Overall scatter timeout (§2.7 principle 3): HTTP 200 + partial results + top-level
    // CX_ERR_SCATTER_TIMEOUT carrying the GEN-Agent 4 fields.
    if (scatter_timed_out) {
        resp.error = MakeCrossNsError(
            CrossNsErrorCode::kScatterTimeout, nlohmann::json::object(),
            "Cross-NS scatter exceeded the overall timeout; returning partial results");
    }
    return resp;
}

}  // namespace cortrix::query
