#include "cortrix/query/degradation_manager.h"

namespace cortrix {

DegradationInfo DegradationManager::Evaluate(
    const RouteResult& vector_result,
    const RouteResult& bm25_result,
    const RouteResult& sql_result,
    const SearchConfig& search_config) const {

    DegradationInfo info;

    // Track enabled routes and their status
    bool vector_enabled = search_config.enable_vector;
    bool bm25_enabled = search_config.enable_bm25;
    bool sql_enabled = search_config.enable_sql;

    bool vector_ok = vector_enabled && vector_result.ok();
    bool bm25_ok = bm25_enabled && bm25_result.ok();
    bool sql_ok = sql_enabled && sql_result.ok();

    // Failed = enabled but not ok and not skipped
    bool vector_failed = vector_enabled &&
        (vector_result.status == RouteStatus::kTimeout || vector_result.status == RouteStatus::kError);
    bool bm25_failed = bm25_enabled &&
        (bm25_result.status == RouteStatus::kTimeout || bm25_result.status == RouteStatus::kError);
    bool sql_failed = sql_enabled &&
        (sql_result.status == RouteStatus::kTimeout || sql_result.status == RouteStatus::kError);

    // Build active/degraded route lists
    if (vector_ok) info.active_routes.push_back("vector");
    if (bm25_ok) info.active_routes.push_back("bm25");
    if (sql_ok) info.active_routes.push_back("sql");

    if (vector_failed) info.degraded_routes.push_back("vector");
    if (bm25_failed) info.degraded_routes.push_back("bm25");
    if (sql_failed) info.degraded_routes.push_back("sql");

    // Determine degradation level
    // Count enabled routes that are expected to succeed
    bool any_enabled = vector_enabled || bm25_enabled || sql_enabled;

    if (!any_enabled) {
        // No routes enabled - not degraded (user's choice)
        info.level = DegradationLevel::kL0;
        info.degraded = false;
        return info;
    }

    if (info.degraded_routes.empty()) {
        // All enabled routes either succeeded or were skipped (not enabled)
        info.level = DegradationLevel::kL0;
        info.degraded = false;
    } else if (sql_failed
               && (vector_ok || bm25_ok)
               && (!vector_enabled || vector_ok)
               && (!bm25_enabled || bm25_ok)) {
        // SQL failed, but all enabled vector/BM25 routes succeeded
        // (and at least one search route is active)
        info.level = DegradationLevel::kL1;
        info.degraded = true;
    } else if (vector_ok || bm25_ok) {
        // At least one of vector/BM25 succeeded
        info.level = DegradationLevel::kL2;
        info.degraded = true;
    } else {
        // All enabled routes failed
        info.level = DegradationLevel::kL3;
        info.degraded = true;
    }

    return info;
}

}  // namespace cortrix
