#pragma once
#include <string>
#include <vector>

#include "cortrix/auth/auth_context.h"            // cortrix::AuthContext
#include "cortrix/common/executor_engine.h"       // cortrix::ExecutorEngine (F-Common scaffold)
#include "cortrix/query/authorize_namespaces.h"   // kDefaultMaxNamespaces
#include "cortrix/query/cross_ns_request.h"
#include "cortrix/query/cross_ns_response.h"
#include "cortrix/query/i_scatter_executor.h"
#include "cortrix/query/permission_service.h"
#include "cortrix/query/query_context.h"
#include "cortrix/reranker.h"                      // cortrix::reranker::IReranker (frozen)
#include "cortrix/retrieval/cross_ns_types.h"

namespace cortrix::query {

/// TraceContext — minimal placeholder for end-to-end tracing (4th
/// param). The full type is owned by the observability subsystem; this layer only needs a
/// trace_id to pass through into QueryContext. Real TraceContext wiring = integration.
struct TraceContext {
    std::string trace_id;
};

/// Timeout knobs (GUC scatter.ns_query_timeout_ms / total_timeout_ms,
/// topic 2.5 dual-layer). Defaults match the spec (5s per-NS / 30s overall). The real
/// values come from the global GUC registry (S4.2, not in W1/W2); ScatterGather
/// takes them by value so it stays standalone-testable with small timeouts.
struct ScatterTimeouts {
    int ns_query_timeout_ms = 5000;   ///< per-NS soft deadline → CX_ERR_NS_TIMEOUT
    int total_timeout_ms = 30000;     ///< overall scatter deadline → CX_ERR_SCATTER_TIMEOUT (partial)
};

/// ScatterGather — the top-level cross-NS query orchestrator.
///
/// Pipeline: AuthorizeNamespaces (steps) → single/multi-NS split (topic 1.6) →
/// per-NS execute (single = inline, multi = engine_->Submit) with dual-layer
/// timeouts (topic 2.5) → Gather (W3) → 8-field response.
///
/// 🚨 Standalone: ScatterGather holds the IReranker* by its **frozen
/// contract** (compiled against reranker.h) and the F-Common ExecutorEngine
/// (#include "cortrix/common/executor_engine.h"). Tests inject a
/// MockIScatterExecutor + MockReranker + MockPermissionService; a real OnnxReranker
/// / the real permission service / the server route are wired later.
///
/// Scope: Execute() does auth + single/multi split (topic 1.6) + dual-timeout
/// scatter (topic 2.5) + Gather (step 3 — sort by cross-NS rerank_score → B simplified
/// DedupeByContentHash → top_k) + the 8-field meta (incl. deduplicated_chunks
/// + warnings) + scatter metrics. content_hash is content-derived standalone;
/// the Block-header source is wired later.
class ScatterGather {
public:
    /// @param executor   the NS executor (V1 = SingleUnitExecutor; NOT owned).
    /// @param engine     shared thread pool for the multi-NS fan-out (NOT owned).
    /// @param reranker   reranker for cross-NS re-rank in Gather (NOT owned;
    ///                  uses it — held now per ctor SoT).
    /// @param perm       permission service for AuthorizeNamespaces (NOT owned).
    ScatterGather(IScatterExecutor* executor,
                  ExecutorEngine* engine,
                  reranker::IReranker* reranker,
                  PermissionService* perm);

    /// Top-level cross-NS query entry (V6 D-33: 4-param signature aligned with
    /// ARCH SoT). Throws CrossNsException for hard failures (auth / too-many /
    /// unauthorized); returns a partial CrossNsResponse (with .error set) for
    /// scatter-timeout (principle 3 — HTTP 200 + partial).
    ///
    /// @param request   query / namespaces / top_k / rerank / filter.
    /// @param auth      BatchCheck input.
    /// @param qctx      optional shared QueryContext; when null, built
    ///                 from request + auth.
    /// @param trace_ctx optional end-to-end tracing context (OBS_SPEC).
    CrossNsResponse Execute(const QueryRequest& request,
                            const AuthContext& auth,
                            const QueryContext* qctx = nullptr,
                            const TraceContext* trace_ctx = nullptr);

    /// Override the dual-layer timeouts (default = spec 5s/30s). S4.2 wires
    /// these from the GUC registry; tests set tiny values to exercise timeout paths.
    void SetTimeouts(const ScatterTimeouts& t) { timeouts_ = t; }
    const ScatterTimeouts& timeouts() const { return timeouts_; }

    /// Override the per-query NS hard cap (default kDefaultMaxNamespaces = 100). This
    /// is the *global* cap (GUC scatter.max_namespaces_per_query); the per-request
    /// effective cap is min(this, AuthContext plan cap) — see S4.3 EffectiveMaxNamespaces.
    void SetMaxNamespaces(int n) { max_namespaces_ = n < 1 ? 1 : n; }
    int max_namespaces() const { return max_namespaces_; }

    /// Apply a GUC-resolved ScatterConfig (S4.2): sets the global NS cap +
    /// dual-layer timeouts in one call. executor.workers / executor.queue_size size
    /// the ExecutorEngine at construction (owned by the caller), so they are not
    /// re-applied here. Declared here; defined in scatter_gather.cpp to keep the
    /// scatter_guc.h dependency out of this header.
    void ApplyConfig(const struct ScatterConfig& cfg);

private:
    /// topic 1.6 single-NS direct path — does NOT enter the ExecutorEngine.
    CrossNsResponse ExecuteSingleNs(const QueryContext& ctx, const std::string& ns);

    /// Multi-NS fan-out — engine_->Submit per NS, dual-layer timeout join.
    CrossNsResponse ExecuteMultiNs(const QueryContext& ctx,
                                   const std::vector<std::string>& nss);

    /// Fold a vector of per-NS results into the response (step 3): success →
    /// ResultItems sorted by cross-NS rerank_score → B simplified DedupeByContentHash
    /// → top_k; failures → meta.namespaces_failed[]; the 8-field meta (incl.
    /// deduplicated_chunks + warnings) is filled; metrics recorded.
    CrossNsResponse GatherAndRerank(std::vector<retrieval::NamespaceQueryResult> results,
                                    const std::vector<std::string>& queried_order,
                                    int top_k,
                                    bool rerank,
                                    bool scatter_timed_out);

    /// Build a QueryContext from request + auth + trace (used when qctx is null).
    QueryContext MakeContext(const QueryRequest& request,
                             const AuthContext& auth,
                             const TraceContext* trace_ctx) const;

    IScatterExecutor* executor_;
    ExecutorEngine* engine_;
    // Held per the ctor SoT; consumed by GatherAndRerank's cross-NS re-rank in
    // Wave 3 (maybe_unused until then so the W1/W2 build stays warning-clean).
    [[maybe_unused]] reranker::IReranker* reranker_;
    PermissionService* perm_;
    ScatterTimeouts timeouts_;
    int max_namespaces_ = kDefaultMaxNamespaces;
};

}  // namespace cortrix::query
