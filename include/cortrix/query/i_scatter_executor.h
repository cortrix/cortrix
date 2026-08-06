#pragma once
#include <string>

#include "cortrix/query/query_context.h"
#include "cortrix/retrieval/cross_ns_types.h"  // cortrix::retrieval::NamespaceQueryResult

namespace cortrix::query {

/// IScatterExecutor — the NS-level scatter execution abstraction.
///
/// Clarification (Lead ruling): this is the **NS-level** interface and is distinct from
/// `cortrix::catalog::IUnitScatterExecutor` (Unit-level, Phase-1 declaration-only,
/// the catalog executor). They must not be conflated or share a name. Phase 1's only impl is
/// `SingleUnitExecutor` (1 NS = 1 Unit). Phase 2 adds `MultiUnitExecutor` (intra-NS
/// multi-Unit scatter, OPEN-3) behind the same interface.
class IScatterExecutor {
public:
    virtual ~IScatterExecutor() = default;

    /// Execute the full query pipeline for ONE namespace
    /// (Vector+BM25 → RRF → Reranker → top_N) and return its RankedChunks.
    ///
    /// @param ctx           shared per-request context (query / top_k / rerank / filter).
    /// @param namespace_id  the NS to query.
    /// @param oversample    OPEN-3 Q4 reservation — Phase 1 default 1.0 (no
    ///                      over-fetch); Phase 2 makes it Unit-count-adaptive so a
    ///                      multi-Unit NS fetches `top_k * oversample` per Unit before
    ///                      intra-NS merge. Phase-1 impls accept it and ignore > 1.0.
    ///
    /// Partial-success contract (topic 2.4): a per-NS failure is NOT thrown — it is
    /// returned in-band as a NamespaceQueryResult with a non-empty `error_code`
    /// (+ category / retryable / retry_after_ms), so ScatterGather can fold it into
    /// `meta.namespaces_failed[]` and still return the other NS's results.
    virtual retrieval::NamespaceQueryResult ExecuteForNamespace(
        const QueryContext& ctx,
        const std::string& namespace_id,
        float oversample = 1.0f) = 0;
};

}  // namespace cortrix::query
