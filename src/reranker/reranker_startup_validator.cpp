#include "cortrix/reranker/reranker_startup_validator.h"

#include <string>

#include "cortrix/reranker/reranker_config.h"
#include "cortrix/reranker/reranker_error.h"
#include "cortrix/reranker/reranker_guc.h"

namespace cortrix::reranker {

Status RerankerStartupValidator::ValidateConfigCompat(int spc_chunk_size,
                                                      int max_seq_length) {
    if (spc_chunk_size <= max_seq_length) {
        return Status::Ok();
    }
    //: fail-fast with the two values + a fix hint in the message. The
    // structured_data object {chunk_size, max_seq_length} is attached by the
    // boundary MakeRerankerError(kConfigMismatch, {...}) at the surface that
    // returns this to a client.
    std::string detail = "spc.chunk_size (" + std::to_string(spc_chunk_size) +
                         ") > reranker.max_seq_length (" + std::to_string(max_seq_length) +
                         "); raise reranker.max_seq_length or lower spc.chunk_size";
    return RerankerStatus(RerankerErrorCode::kConfigMismatch, detail);
}

Status RerankerStartupValidator::ValidateStartupFromGlobalConfig(const IGlobalConfig& cfg) {
    // reranker.max_seq_length: range-validated through the full RerankerConfig load
    // (rejects an out-of-range max_seq_length before the compat comparison).
    RerankerConfig rc;
    Status rs = RerankerGuc::LoadFromGlobalConfig(cfg, &rc);
    if (!rs.ok()) return rs;

    // spc.chunk_size: defaults to 512 when absent, range-validated.
    Result<int> spc = RerankerGuc::LoadSpcChunkSize(cfg);
    if (!spc.ok()) return spc.status();

    return ValidateConfigCompat(spc.value(), rc.max_seq_length);
}

}  // namespace cortrix::reranker
