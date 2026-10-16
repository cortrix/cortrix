#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include <sqlite3.h>

#include "cortrix/common/result.h"
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

// --- addendum §3.8 W2: dual-vector ANN label mapping (F35-9 B) ----------------
// The contextualized embedding enters P-HNSW as its OWN point under a derived
// label (not a blocks row). contextual_vec_labels (F35SchemaProvider V2) maps
// that label back to the owning child so the query side can resolve an ANN hit
// on a contextual point into a contextualized-path RRF vote (F35 §9.1).

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

/// Remove the label row (idempotent: an absent row is already gone). The caller
/// marks the index point deleted first, so a failure here leaves a row the next
/// reconciliation retries rather than a point nothing can resolve.
Status DeleteContextualVecLabel(sqlite3* db, uint64_t label);

// --- label-only contextual debt ----------------------------------------------
// When a child's contextual columns are complete but its index label row is
// missing, the provider work for the member is done: the point can be re-added
// from the stored vector and the label row written, without calling the provider.
// The enrichment audit (which owes such a chunk) and the backfill worker (which
// repairs it) must agree on exactly which rows that is, so both evaluate the one
// SQL expression below — never two hand-written variants of it — and on which of
// those rows a label is expected at all, which is the ingest's index gate
// (index_gates.h: IndexPointsExpected over the document's level and the cleaner's
// skip-index mark, read as MetadataSkipsIndex / kBlockSkipsIndexSql).

/// SQL over one `blocks` row: the contextual member's provider work is complete
/// and the stored vector is usable (status generated, a non-empty float32 blob).
/// As a SELECT column it can be NULL (a NULL status), which readers take as false:
/// read it with sqlite3_column_int (NULL reads as 0), never with a type check.
inline constexpr char kBlockHasStoredContextualVectorSql[] =
    "(contextualized_status = 1 AND contextualized_embedding IS NOT NULL"
    " AND length(contextualized_embedding) > 0"
    " AND length(contextualized_embedding) % 4 = 0)";

/// The stored contextual vector of a block that satisfies
/// kBlockHasStoredContextualVectorSql, decoded from its float32 blob. NotFound
/// when the block does not satisfy it (or does not exist); any other status is a
/// store error. The width is not checked here: the caller compares it with the
/// index it is about to add the point to.
Result<std::vector<float>> ReadStoredContextualVector(sqlite3* db, uint64_t block_id);

}  // namespace cortrix::spc
