#pragma once
#include <cstdint>
#include <string>

#include <sqlite3.h>

#include "cortrix/common/result.h"
#include "cortrix/common/status.h"
#include "cortrix/spc_enricher.h"  // EnrichResult (contextualized_* fields)

namespace cortrix::spc {

// I2 — persist the contextualized_* columns F35SchemaProvider added to
// the per-Unit `blocks` table (embedding / contextualized_text /
// contextualized_embedding / contextualized_status). A free function over a
// sqlite3* (same shape as the enricher's WriteEnrichment) so it unit-tests against an
// in-memory DB with the contextual migration applied. The live wiring (which block_id,
// transaction boundary with the block write) is the SPC pipeline's Stage-5
// write phase.
//
// Only child rows carry contextualized data (the 4 columns are written
// only for block_type=child rows). A merged EnrichResult with no contextual output
// (contextualized_status == 0, no engaged optionals) is a no-op — the columns stay
// at their DEFAULT (NULL / 0), so a chain without this stage or an L1 default leaves blocks
// untouched.
//
// The two BLOB columns store the dense vectors as little-endian float32 packed
// contiguously (the same byte layout the embedder produces); the read path
// (the 5-path RRF) decodes them symmetrically.
Status WriteContextualized(sqlite3* db, uint64_t block_id, const EnrichResult& result);

// --- dual-vector ANN label mapping ------------------------------------------
// The contextualized embedding enters P-HNSW as its OWN point under a derived
// label (not a blocks row). contextual_vec_labels (F35SchemaProvider V2) maps
// that label back to the owning child so the query side can resolve an ANN hit
// on a contextual point into a contextualized-path RRF vote.

struct ContextualVecLabelRow {
    uint64_t label = 0;     ///< derived P-HNSW label of the contextual point
    uint64_t block_id = 0;  ///< owning child block
    std::string child_id;   ///< owning child ULID
};

/// Deterministic ANN label of a child's contextual point (idempotent across
/// re-ingest / backfill): HashChildIdToBlockId(child_id + ":ctx").
uint64_t DeriveContextualVecLabel(const std::string& child_id);

/// Upsert the label → (block_id, child_id) mapping (idempotent — the label is a
/// deterministic derivation, so re-ingest/backfill rewrites the same row).
Status WriteContextualVecLabel(sqlite3* db, const ContextualVecLabelRow& row);

/// Resolve one ANN label. Ok(row) on hit; CX_ERR-style NotFound when absent
/// (the caller treats an unknown label as a dropped hit, same as a missing block).
Result<ContextualVecLabelRow> GetContextualVecLabel(sqlite3* db, uint64_t label);

}  // namespace cortrix::spc
