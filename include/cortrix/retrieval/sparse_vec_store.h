#pragma once
#include <cstdint>

#include <sqlite3.h>

#include "cortrix/common/status.h"
#include "cortrix/retrieval/sparse_codec.h"  // SparseVector

namespace cortrix::retrieval {

// Persist a child chunk's SPLADE sparse vector into the
// per-Unit blocks.sparse_vec BLOB column (added by F40SchemaProvider). A free
// function over a sqlite3* (same shape as WriteContextualized /
// WriteEnrichment) so it unit-tests against an in-memory DB with the sparse migration
// applied. The live wiring (which block_id, the transaction boundary with the
// block write) is the SPC pipeline's Stage-5 write phase.
//
// Only child rows carry a sparse vector (the column is written only for
// block_type=child rows). An empty `vec` (a "dead" chunk with no active sparse
// terms) writes SQL NULL — never an all-zero BLOB — so the read path's
// has_sparse_vec test and the dead-chunk sentinel agree. Serialization overflow
// (term_id >= 65536) surfaces as CX_ERR_F40_SPARSE_SERIALIZE_FAILED.
Status WriteSparseVec(sqlite3* db, uint64_t block_id, const SparseVector& vec);

}  // namespace cortrix::retrieval
