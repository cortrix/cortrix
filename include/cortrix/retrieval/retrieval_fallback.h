#pragma once
#include <string>
#include <vector>

#include "cortrix/retrieval/types.h"  // RankedChunk

namespace cortrix::query {
struct QueryContext;  // fwd-decl — Fallback only takes a const reference
}  // namespace cortrix::query

namespace cortrix::retrieval {

/// IRetrievalFallback — the Phase-1 interface reservation for CRAG "Incorrect"
/// corrective retrieval (F37 §5.3 / borrowed-5). When F37 evaluates a query as
/// Incorrect, Phase 1 only records it and returns the degraded reranker results;
/// Phase 2 plugs a WebSearchFallback (Bing / Brave / Tavily) behind this same
/// interface to actually fetch corrective context.
class IRetrievalFallback {
public:
    virtual ~IRetrievalFallback() = default;

    /// Called when F37's verdict is "incorrect". Phase 1's only implementation
    /// (NullRetrievalFallback) returns the original chunks unchanged. Phase 2
    /// implementations may issue an external search and return new RankedChunk[].
    virtual std::vector<RankedChunk> Fallback(
        const query::QueryContext& ctx,
        const std::vector<RankedChunk>& original_chunks) = 0;

    /// Stable fallback name (logging / explain). e.g. "null_fallback".
    virtual std::string Name() const = 0;
};

/// NullRetrievalFallback — the Phase-1 default (no-op). Returns the original chunks
/// untouched (§5.3): Phase 1 does not perform Web corrective retrieval.
class NullRetrievalFallback : public IRetrievalFallback {
public:
    std::vector<RankedChunk> Fallback(
        const query::QueryContext& /*ctx*/,
        const std::vector<RankedChunk>& original_chunks) override {
        return original_chunks;  // Phase 1: return raw results only
    }
    std::string Name() const override { return "null_fallback"; }
};

}  // namespace cortrix::retrieval
