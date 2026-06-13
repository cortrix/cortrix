#pragma once
#include <sqlite3.h>

#include "cortrix/common/status.h"
#include "cortrix/spc_enricher.h"  // EnrichResult (contextualized_* fields)

namespace cortrix::spc {

// I2 (F35 §4.1) — persist the contextualized_* columns F35SchemaProvider added to
// the per-Unit `blocks` table (embedding / contextualized_text /
// contextualized_embedding / contextualized_status). A free function over a
// sqlite3* (same shape as F03's WriteEnrichment) so it unit-tests against an
// in-memory DB with the F35 migration applied. The live wiring (which block_id,
// transaction boundary with the F09 block write) is the SPC pipeline's Stage-5
// write phase.
//
// Only child rows carry contextualized data (F35 §4.1: the 4 columns are written
// only for block_type=child rows). A merged EnrichResult with no F35 output
// (contextualized_status == 0, no engaged optionals) is a no-op — the columns stay
// at their DEFAULT (NULL / 0), so a non-F35 chain or an L1 default leaves blocks
// untouched.
//
// The two BLOB columns store the dense vectors as little-endian float32 packed
// contiguously (the same byte layout the embedder produces); the read path
// (F40 5-path RRF, Wave Q) decodes them symmetrically.
Status WriteContextualized(sqlite3* db, uint64_t block_id, const EnrichResult& result);

}  // namespace cortrix::spc
