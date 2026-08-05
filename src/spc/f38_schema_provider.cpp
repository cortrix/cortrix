#include "cortrix/spc/f38_schema_provider.h"

#include <string>

namespace cortrix::spc {

Status F38SchemaProvider::Migrate(sqlite3* /*db*/, int from_ver, int to_ver) {
    // Phase 1: 0 → 1 is an init no-op — hype_question Blocks (block_type=16) reuse
    // the block-header-owned per-Unit `blocks` table's existing block_type column; HyPE owns
    // no extra table or column. Also accept an already-current (n → n) call
    // defensively (mirrors F02SchemaProvider).
    if ((from_ver == 0 && to_ver == 1) || from_ver == to_ver) {
        return Status::Ok();
    }
    // Phase 2 (independent P-HNSW / Block versioning, §14) is the only future
    // step; until it is defined an unexpected version jump is a mismatch. The
    // CX_ERR_F38_SCHEMA_VERSION_MISMATCH identity (registered in the HyPE error
    // registry) survives via the token prefix on the Status message.
    return Status::InvalidArgument(
        "CX_ERR_F38_SCHEMA_VERSION_MISMATCH: F38 unsupported migration " +
        std::to_string(from_ver) + " -> " + std::to_string(to_ver));
}

}  // namespace cortrix::spc
