#include "cortrix/reranker/reranker_schema_provider.h"

#include <string>

namespace cortrix::reranker {

Status F02SchemaProvider::Migrate(sqlite3* /*db*/, int from_ver, int to_ver) {
    // Phase 1: 0 → 1 is an init no-op (reranker_config is supplied by the catalog base
    // schema; the reranker owns no extra column). Also accept an already-current (1 → 1)
    // call defensively.
    if ((from_ver == 0 && to_ver == 1) || from_ver == to_ver) {
        return Status::Ok();
    }
    // Phase 2 placeholder: from_ver==1 && to_ver==2 → reranker_config V1→V2
    // evolution (§2.4 adds the model / score_threshold per-NS keys). Until then,
    // an unexpected version step is an error.
    return Status::InvalidArgument(
        "CX_ERR_SCHEMA_VERSION_MISMATCH: F02 unsupported migration " +
        std::to_string(from_ver) + " -> " + std::to_string(to_ver));
}

}  // namespace cortrix::reranker
