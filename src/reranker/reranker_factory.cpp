#include "cortrix/reranker.h"

#include "cortrix/reranker/onnx_reranker.h"

namespace cortrix::reranker {

IReranker* CreateReranker(const RerankerConfig& config, store::ChunkStore* chunk_store) {
    // V1: only OnnxReranker. (reranker.type onnx/llamacpp/api is a GUC, but V1
    // implements onnx only.)
    auto* reranker = new OnnxReranker(config, chunk_store);
    Status s = reranker->Init();
    if (!s.ok()) {
        // Init's fail-fast path (S1.3) returns a domain error; surface it by
        // returning nullptr so the caller can abort startup.
        delete reranker;
        return nullptr;
    }
    return reranker;
}

}  // namespace cortrix::reranker
