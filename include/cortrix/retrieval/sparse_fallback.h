#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "cortrix/common/status.h"
#include "cortrix/retrieval/sparse_codec.h"
#include "cortrix/retrieval/sparse_retriever.h"
#include "cortrix/retrieval/sparse_rrf.h"

namespace cortrix::retrieval {

/// Default write-time serialize retry count (F40 §7.1 L1: "retry N=3").
inline constexpr int kSparseSerializeRetries = 3;

/// Outcome of the L1 write-time serialize path (F40 §7.1). On success the BLOB is
/// present and the F09 has_sparse_vec bit should be SET. On a serialize failure
/// (after N retries) the chunk degrades to dense+FTS5 only: blob is empty,
/// has_sparse_vec must be CLEARED, sparse_vec column written NULL — NOT an error
/// that fails the chunk write (only a *dense+sparse both* inference failure does
/// that, which is handled by the caller raising CX_ERR_F40_INFERENCE_FAILED).
struct L1SerializeResult {
    bool serialized = false;          ///< true → use `blob`; false → write NULL
    std::vector<uint8_t> blob;        ///< the sparse_vec BLOB when serialized
    bool set_has_sparse_vec = false;  ///< F09 flags_ext bit decision
    int attempts = 0;                 ///< how many serialize attempts were made
};

/// L1 write-time sparse serialization with the §7.1 degrade policy. Empty `vec`
/// (dead chunk, §6.5) → serialized=false, write NULL, clear the flag (no error,
/// no retries). A non-empty vec is serialized; on a serializer failure it retries
/// up to `retries` times, then degrades to NULL (serialized=false) rather than
/// failing the write. SerializeSparseVec is deterministic, so a retry only helps
/// a transient (it never does for a genuine out-of-range id) — the loop matches
/// the §7.1 contract and surfaces the attempt count for the metric.
L1SerializeResult L1SerializeSparseVec(const SparseVector& vec,
                                       int retries = kSparseSerializeRetries);

/// L2 query-time fallback decision (F40 §7.2). Given whether the sparse path is
/// available (e.g. ISparseRetriever::IsAvailable() / last_search_failed()),
/// returns the explain payload: on the normal path via_path="sparse"; on
/// fallback via_path="fallback_dense_fts5_hype" + fallback_used=true. `sparse_top_k`
/// is the resolved S7 value (recorded for explain).
SparseExplain DecideL2Fallback(bool sparse_available, int resolved_top_k);

/// Build the 5-path RRF input for the L2 fallback path: identical to the normal
/// input but with the sparse list dropped (empty), so FuseFivePathRrf degrades to
/// dense + contextualized + fts5 + hype (the §7.2 "4-path RRF"). The caller passes
/// the lists it has; this just zeroes the sparse one to make the intent explicit
/// + testable.
FivePathInput DropSparsePath(FivePathInput input);

}  // namespace cortrix::retrieval
