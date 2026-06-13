#include "cortrix/query/query_pipeline.h"
#include <future>
#include <chrono>

namespace cortrix {

QueryPipeline::QueryPipeline(VectorSearcher& vec_searcher,
                             BM25Searcher& bm25_searcher,
                             RRFFusion& fusion,
                             IntentClassifier& classifier,
                             PostFilter& filter,
                             DegradationManager& degradation,
                             SqlExecutor& sql_executor,
                             int64_t merge_reserve_ms)
    : vec_searcher_(vec_searcher)
    , bm25_searcher_(bm25_searcher)
    , fusion_(fusion)
    , classifier_(classifier)
    , filter_(filter)
    , degradation_(degradation)
    , sql_executor_(sql_executor) {
    merge_reserve_ms_ = (merge_reserve_ms > 0) ? merge_reserve_ms : kDefaultMergeReserveMs;
}

QueryResponse QueryPipeline::Execute(const QueryRequest& request) {
    auto start = std::chrono::steady_clock::now();

    // Fast path: doc_id pre-filter — bypass HNSW/BM25 entirely.
    // Fetches all blocks for the specified document directly from SQLite and
    // returns them in chunk_index order.  Used by rag_engine filename/latest-doc
    // fallbacks where the target document is known but query text has low cosine
    // similarity to document content.
    if (!request.filters.doc_id.empty()) {
        auto results = filter_.FetchByDocId(
            request.filters.doc_id, request.filters, request.top_k);

        auto elapsed = std::chrono::steady_clock::now() - start;
        int64_t latency_ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();

        QueryResponse response;
        response.results = std::move(results);
        response.meta.total_results = static_cast<int>(response.results.size());
        response.meta.latency_ms = latency_ms;
        response.meta.routes_used = {"doc_id"};
        response.meta.degraded = false;
        response.meta.degradation_level = 0;
        return response;
    }

    int64_t route_timeout_us = CalculateRouteTimeout(request.timeout_ms) * 1000;

    // Step 1: Launch three tasks in parallel
    auto vector_future = std::async(std::launch::async, [&]() -> RouteResult {
        if (!request.search_config.enable_vector)
            return RouteResult{"vector", RouteStatus::kSkipped, "", 0, {}};
        return vec_searcher_.Search(request.query, request.top_k, route_timeout_us);
    });

    auto bm25_future = std::async(std::launch::async, [&]() -> RouteResult {
        if (!request.search_config.enable_bm25)
            return RouteResult{"bm25", RouteStatus::kSkipped, "", 0, {}};
        return bm25_searcher_.Search(request.query, request.top_k, route_timeout_us);
    });

    auto intent_future = std::async(std::launch::async, [&]() -> QueryIntent {
        return classifier_.Classify(request.query, route_timeout_us);
    });

    // Step 2: Collect results
    RouteResult vector_result = vector_future.get();
    RouteResult bm25_result = bm25_future.get();
    QueryIntent intent = intent_future.get();

    // SQL route: execute via SqlExecutor when intent indicates SQL need.
    // Design spec (ARCHITECTURE_DEEP_DESIGN.md Section 3.4.2):
    //   - kSql or kHybrid intent -> fire SQL route
    //   - kSemantic -> skip SQL route
    //   - search_config.enable_sql == false -> skip
    // MVP: SqlExecutorStub always returns kSkipped for graceful degradation.
    RouteResult sql_result;
    SqlResult sql_data;
    if (request.search_config.enable_sql &&
        (intent == QueryIntent::kSql || intent == QueryIntent::kHybrid)) {
        sql_result = sql_executor_.Execute(
            request.query, request.namespace_name, route_timeout_us, sql_data);
    } else {
        sql_result.route_name = "sql";
        sql_result.status = RouteStatus::kSkipped;
    }

    // Step 3: Degradation evaluation (before fusion, to detect L3 early)
    auto deg = degradation_.Evaluate(
        vector_result, bm25_result, sql_result, request.search_config);

    // If all routes failed (L3), return error response immediately
    if (deg.level == DegradationLevel::kL3) {
        auto elapsed = std::chrono::steady_clock::now() - start;
        int64_t latency_ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();

        QueryResponse response;
        response.sql_result.has_result = false;
        response.meta.total_results = 0;
        response.meta.latency_ms = latency_ms;
        response.meta.routes_used = deg.active_routes;
        response.meta.degraded = true;
        response.meta.degraded_routes = deg.degraded_routes;
        response.meta.degradation_level = static_cast<int>(deg.level);
        response.has_error = true;
        response.error_message = "All search routes failed";
        return response;
    }

    // Step 4: RRF fusion (only successful routes contribute scores)
    std::vector<RouteResult> routes = {vector_result, bm25_result};
    auto scored_blocks = fusion_.Merge(routes);

    // Step 5: Post-filter + load details
    auto results = filter_.Apply(scored_blocks, request.filters, request.top_k);

    // Measure latency at the very end (includes fusion + post-filter time)
    auto elapsed = std::chrono::steady_clock::now() - start;
    int64_t latency_ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();

    // Assemble response
    QueryResponse response;
    response.results = std::move(results);
    response.sql_result = sql_data;  // Populated by SqlExecutor (stub returns has_result=false)
    response.meta.total_results = static_cast<int>(response.results.size());
    response.meta.latency_ms = latency_ms;
    response.meta.routes_used = deg.active_routes;
    response.meta.degraded = deg.degraded;
    response.meta.degraded_routes = deg.degraded_routes;
    response.meta.degradation_level = static_cast<int>(deg.level);

    return response;
}

int64_t QueryPipeline::CalculateRouteTimeout(int api_timeout_ms) const {
    // Reserve merge_reserve_ms_ for RRF fusion + post-filter + response serialization
    int64_t route_timeout_ms = static_cast<int64_t>(api_timeout_ms) - merge_reserve_ms_;
    if (route_timeout_ms < kMinRouteTimeoutMs) route_timeout_ms = kMinRouteTimeoutMs;
    return route_timeout_ms;
}

}  // namespace cortrix
