#include "cortrix/chunker/chunker_startup_validator.h"

#include <string>

#include "cortrix/chunker/chunker_errors.h"
#include "cortrix/chunker/chunker_guc.h"
#include "cortrix/common/result.h"
#include "cortrix/reranker/reranker_guc.h"  // reranker.max_seq_length GUC key + range

namespace cortrix::chunker {

Status ChunkerStartupValidator::ValidateChildSizeCompat(int child_size, int max_seq_length) {
    if (child_size > max_seq_length) {
        // CX_ERR_CHUNK_SIZE_INVALID (§ 5.1). The boundary re-inflates this into the
        // full Agent-friendly body (retryable=false / category=permanent /
        // structured_data {child_size, max_seq_length}) at the surfacing call site.
        return ChunkStatus(
            StatusCode::kInvalidArgument, chunk_errors::kSizeInvalid,
            "child_size " + std::to_string(child_size) +
                " > reranker.max_seq_length " + std::to_string(max_seq_length));
    }
    return Status::Ok();
}

Status ChunkerStartupValidator::ValidateStartupFromGlobalConfig(const IGlobalConfig& cfg) {
    // 1) Load + range-validate the chunker config (an out-of-range chunker GUC
    //    surfaces as that GUC's InvalidArgument before the compat check).
    ChunkerConfig chunker_cfg;
    Status s = ChunkerGuc::LoadFromConfig(cfg, &chunker_cfg);
    if (!s.ok()) return s;

    // 2) Read reranker.max_seq_length (range-validated against the reranker's own
    //    [128, 8192]); absent → reranker default 512.
    int max_seq_length = 512;
    Result<int> r = cfg.GetInt(reranker::guc::kMaxSeqLength);
    if (r.ok()) {
        Status rs = reranker::RerankerGuc::ValidateRange(
            reranker::guc::kMaxSeqLength, r.value(),
            reranker::guc::kMaxSeqLenMin, reranker::guc::kMaxSeqLenMax);
        if (!rs.ok()) return rs;
        max_seq_length = r.value();
    }

    // 3) F34-1 compat gate.
    return ValidateChildSizeCompat(chunker_cfg.child_size, max_seq_length);
}

}  // namespace cortrix::chunker
