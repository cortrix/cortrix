#include "cortrix/connector/watcher_schema_provider.h"

namespace cortrix::connector {

Status F21SchemaProvider::Migrate(sqlite3* /*db*/, int from_ver, int to_ver) {
    // Phase 1: 0 → 1 is an init no-op. namespaces.watcher_config JSONB is created
    // by the F12 base schema (catalog_schema.cpp) and F21 owns no extra column,
    // so there is no DDL to emit here. Also accept an already-current (1 → 1)
    // call defensively.
    if ((from_ver == 0 && to_ver == 1) || from_ver == to_ver) {
        return Status::Ok();
    }
    // Phase 2 placeholder: from_ver==1 && to_ver==2 → watcher_config V1→V2
    // evolution (TD-WATCHER-OPTIONS activates the 4 per-NS JSONB keys). Until
    // then, an unexpected version step is an error.
    return Status::InvalidArgument(
        "CX_ERR_SCHEMA_VERSION_MISMATCH: F21 unsupported migration " +
        std::to_string(from_ver) + " -> " + std::to_string(to_ver));
}

}  // namespace cortrix::connector
