#include "cortrix/retrieval/sparse_fallback.h"

namespace cortrix::retrieval {

L1SerializeResult L1SerializeSparseVec(const SparseVector& vec, int retries) {
    L1SerializeResult r;
    // Dead chunk (§6.5): no sparse signal → write NULL, clear the flag, no error.
    if (vec.empty()) {
        r.serialized = false;
        r.set_has_sparse_vec = false;
        r.attempts = 0;
        return r;
    }

    // §7.1: try to serialize; on failure retry up to `retries` more times, then
    // degrade to NULL (dense+FTS5 only) rather than failing the chunk write.
    int max_attempts = (retries < 0 ? 0 : retries) + 1;  // initial + retries
    for (int attempt = 1; attempt <= max_attempts; ++attempt) {
        bool ok = false;
        std::vector<uint8_t> blob = SerializeSparseVec(vec, &ok);
        r.attempts = attempt;
        if (ok) {
            r.serialized = true;
            r.set_has_sparse_vec = true;
            r.blob = std::move(blob);
            return r;
        }
    }
    // All attempts failed → transparent degrade (sparse_vec=NULL, flag cleared).
    r.serialized = false;
    r.set_has_sparse_vec = false;
    return r;
}

SparseExplain DecideL2Fallback(bool sparse_available, int resolved_top_k) {
    SparseExplain ex;
    ex.sparse_top_k = resolved_top_k;
    if (sparse_available) {
        ex.via_path = "sparse";
        ex.fallback_used = false;
        ex.fallback_paths = {"sparse", "dense", "fts5"};
    } else {
        // §7.2 L2: transparent degrade to dense + FTS5 (+ hype) — drop sparse.
        ex.via_path = "fallback_dense_fts5_hype";
        ex.fallback_used = true;
        ex.fallback_paths = {"dense", "fts5", "hype"};
    }
    return ex;
}

FivePathInput DropSparsePath(FivePathInput input) {
    input.sparse.clear();  // §7.2: 5-path → 4-path (no sparse)
    return input;
}

}  // namespace cortrix::retrieval
