#pragma once

#include "cortrix/common/i_global_config.h"
#include "cortrix/common/status.h"

namespace cortrix::reranker {

/// Reranker startup-time config validator.
///
/// Validates the SPC↔Reranker compatibility invariant `spc.chunk_size ≤
/// reranker.max_seq_length` so an SPC chunk can never exceed what the reranker can
/// process. On violation it returns a Status carrying the CX_ERR_CONFIG_MISMATCH
/// identity (re-inflatable to the full Agent-friendly body via
/// MakeRerankerError); cortrix-server's startup aborts on a non-OK Status
/// (fail-fast, mirrors the ONNX StartupValidator).
///
/// Standalone (D3): this validates *given* values. Reading the live GUCs
/// (spc.chunk_size, reranker.max_seq_length) from IGlobalConfig and invoking this
/// in the live cortrix-server startup sequence is cross-Feature wiring deferred to
/// later (same discipline as StartupValidator::CollectRegisteredOnnxModels).
class RerankerStartupValidator {
public:
    /// OK iff spc_chunk_size <= max_seq_length. Otherwise CX_ERR_CONFIG_MISMATCH
    /// with both values in the message (structured_data {chunk_size,
    /// max_seq_length} is attached by the boundary MakeRerankerError at the call
    /// site that surfaces this to a client).
    static Status ValidateConfigCompat(int spc_chunk_size, int max_seq_length);

    /// Read spc.chunk_size + reranker.max_seq_length from `cfg` (via RerankerGuc)
    /// and run ValidateConfigCompat. Each GUC is range-validated first (an
    /// out-of-range value surfaces as that GUC's InvalidArgument before the compat
    /// check). This is the standalone IGlobalConfig entry point; invoking it inside
    /// the live cortrix-server startup sequence is wired separately.
    static Status ValidateStartupFromGlobalConfig(const IGlobalConfig& cfg);
};

}  // namespace cortrix::reranker
