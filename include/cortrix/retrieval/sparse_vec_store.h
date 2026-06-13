#pragma once
#include <cstdint>

#include <sqlite3.h>

#include "cortrix/common/status.h"
#include "cortrix/retrieval/sparse_codec.h"  // SparseVector

namespace cortrix::retrieval {

// F40 §4.2 / §6.1 — persist a child chunk's SPLADE sparse vector into the
// per-Unit blocks.sparse_vec BLOB column (added by F40SchemaProvider). A free
// function over a sqlite3* (same shape as F35's WriteContextualized / F03's
// WriteEnrichment) so it unit-tests against an in-memory DB with the F40 migration
// applied. The live wiring (which block_id, the F25 transaction boundary with the
// F09 block write) is the SPC pipeline's Stage-5 write phase.
//
// Only child rows carry a sparse vector (F40-2: the column is written only for
// block_type=child rows). An empty `vec` (a "dead" chunk with no active sparse
// terms, F40 §6.5) writes SQL NULL — never an all-zero BLOB — so the read path's
// has_sparse_vec test and the dead-chunk sentinel agree. Serialization overflow
// (term_id >= 65536) surfaces as CX_ERR_F40_SPARSE_SERIALIZE_FAILED.
Status WriteSparseVec(sqlite3* db, uint64_t block_id, const SparseVector& vec);

}  // namespace cortrix::retrieval
