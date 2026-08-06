#pragma once
#include "cortrix/chunker/chunker_config.h"
#include "cortrix/common/i_global_config.h"
#include "cortrix/common/status.h"

// Chunker startup-time config validator.
//
// Enforces the chunker↔reranker invariant `spc.chunker.child_size ≤
// reranker.max_seq_length` so a Child can never exceed what the reranker can
// process. On violation it returns CX_ERR_CHUNK_SIZE_INVALID with both values;
// cortrix-server's startup aborts on a non-OK Status (fail-fast, mirrors
// reranker_startup_validator / the ONNX StartupValidator). This is the chunker side of the
// same compat gate the reranker enforces for spc.chunk_size.
namespace cortrix::chunker {

class ChunkerStartupValidator {
public:
    /// OK iff child_size <= max_seq_length. Otherwise CX_ERR_CHUNK_SIZE_INVALID
    /// with both values in the message (structured_data {child_size,
    /// max_seq_length} is attached by the boundary at the surfacing call site).
    static Status ValidateChildSizeCompat(int child_size, int max_seq_length);

    /// Read spc.chunker.* (via ChunkerGuc, range-validated first) +
    /// reranker.max_seq_length from `cfg`, then run ValidateChildSizeCompat.
    /// Standalone IGlobalConfig entry point; invoking it inside the live
    /// cortrix-server startup sequence is integration (mirrors reranker validator).
    static Status ValidateStartupFromGlobalConfig(const IGlobalConfig& cfg);
};

}  // namespace cortrix::chunker
