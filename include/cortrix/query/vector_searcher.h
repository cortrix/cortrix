#pragma once
#include <cstdint>
#include <string>
#include "cortrix/query/route_result.h"
#include "cortrix/store/iindex.h"

namespace cortrix {

class OnnxEmbedder;

class VectorSearcher {
public:
    /// @param vec_index: per-namespace F01 vector index (façade.vec_index(), = bundle.index)
    /// @param embedder: F03 OnnxEmbedder instance (global shared)
    VectorSearcher(cortrix::store::IIndex& vec_index, OnnxEmbedder& embedder);

    /// Execute vector search
    /// 1. Embed query_text via embedder.Embed()
    /// 2. Call vec_index.Search(query_vec, top_k * 3)
    /// 3. Convert (block_id, distance) hits -> RouteResult
    ///
    /// @param query_text: raw query text
    /// @param top_k: result count (oversampled to top_k * 3 for post-filter)
    /// @param timeout_us: route-level timeout (microseconds)
    /// @return RouteResult (route_name = "vector")
    RouteResult Search(const std::string& query_text, int top_k, int64_t timeout_us);

private:
    cortrix::store::IIndex& vec_index_;
    OnnxEmbedder& embedder_;
};

}  // namespace cortrix
