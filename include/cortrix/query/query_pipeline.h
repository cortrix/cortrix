#pragma once
#include <cstdint>
#include "cortrix/query/query_request.h"
#include "cortrix/query/query_response.h"
#include "cortrix/query/vector_searcher.h"
#include "cortrix/query/bm25_searcher.h"
#include "cortrix/query/rrf_fusion.h"
#include "cortrix/query/intent_classifier.h"
#include "cortrix/query/post_filter.h"
#include "cortrix/query/degradation_manager.h"
#include "cortrix/query/sql_executor.h"

namespace cortrix {

class QueryPipeline {
public:
    /// Default merge reserve (ms) for RRF fusion + post-filter + serialization
    static constexpr int64_t kDefaultMergeReserveMs = 200;

    /// @param vec_searcher: vector searcher (per-namespace)
    /// @param bm25_searcher: BM25 searcher (per-namespace)
    /// @param fusion: RRF fusion engine
    /// @param classifier: intent classifier (global shared)
    /// @param filter: post-filter (per-namespace)
    /// @param degradation: degradation manager
    /// @param sql_executor: SQL executor (global shared, MVP uses SqlExecutorStub)
    /// @param merge_reserve_ms: reserve time in ms for fusion/filter/serialization (default 200)
    QueryPipeline(VectorSearcher& vec_searcher,
                  BM25Searcher& bm25_searcher,
                  RRFFusion& fusion,
                  IntentClassifier& classifier,
                  PostFilter& filter,
                  DegradationManager& degradation,
                  SqlExecutor& sql_executor,
                  int64_t merge_reserve_ms = kDefaultMergeReserveMs);

    /// Execute full query pipeline
    /// 1. Intent classification (parallel, non-blocking)
    /// 2. vector + BM25 parallel execution
    /// 3. Wait for SQL based on intent (not executed yet; reserved)
    /// 4. RRF fusion
    /// 5. Post-filter + load details
    /// 6. Degradation evaluation
    /// 7. Assemble QueryResponse
    ///
    /// @param request: query request
    /// @return QueryResponse
    QueryResponse Execute(const QueryRequest& request);

private:
    /// Calculate route timeout: API timeout - merge_reserve_ms
    int64_t CalculateRouteTimeout(int api_timeout_ms) const;

    /// Minimum route timeout in ms (floor for CalculateRouteTimeout)
    static constexpr int64_t kMinRouteTimeoutMs = 100;

    /// Default reserve in ms for RRF fusion + post-filter + serialization.
    /// Override via constructor parameter merge_reserve_ms if needed.
    int64_t merge_reserve_ms_ = kDefaultMergeReserveMs;

    VectorSearcher& vec_searcher_;
    BM25Searcher& bm25_searcher_;
    RRFFusion& fusion_;
    IntentClassifier& classifier_;
    PostFilter& filter_;
    DegradationManager& degradation_;
    SqlExecutor& sql_executor_;
};

}  // namespace cortrix
