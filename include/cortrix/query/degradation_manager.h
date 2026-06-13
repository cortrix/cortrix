#pragma once
#include <vector>
#include <string>
#include "cortrix/query/route_result.h"
#include "cortrix/query/query_request.h"

namespace cortrix {

/// Degradation level
enum class DegradationLevel {
    kL0 = 0,   // Full function: all enabled routes succeed
    kL1 = 1,   // No SQL: LLM unavailable or SQL route failed
    kL2 = 2,   // Single route: only vector or only BM25
    kL3 = 3,   // 503: all routes failed
};

struct DegradationInfo {
    DegradationLevel level = DegradationLevel::kL0;
    bool degraded = false;
    std::vector<std::string> degraded_routes;   // failed route names
    std::vector<std::string> active_routes;     // succeeded route names
};

class DegradationManager {
public:
    /// Evaluate degradation level based on route results
    ///
    /// @param vector_result: vector search result
    /// @param bm25_result: BM25 search result
    /// @param sql_result: SQL search result (F07 always kSkipped)
    /// @param search_config: client route switches (disabled routes don't count)
    /// @return DegradationInfo
    DegradationInfo Evaluate(
        const RouteResult& vector_result,
        const RouteResult& bm25_result,
        const RouteResult& sql_result,
        const SearchConfig& search_config) const;
};

}  // namespace cortrix
