#pragma once
#include <cstdint>
#include <map>
#include <vector>

#include "cortrix/common/result.h"
#include "cortrix/common/status.h"

namespace cortrix::retrieval {

/// A SPLADE sparse vector: BGE-M3 lexical_weights as term_id → weight.
/// This is the net-new retrieval-layer type the inverted index + RRF consume;
/// it is the same shape the OnnxEmbedder produces (EmbedWithSparseResult.sparse)
/// — kept as a named struct here (not a bare map) so the interface contract is
/// explicit and so weights/terms can grow fields in Phase 2 without churn.
struct SparseVector {
    std::map<uint32_t, float> terms;  ///< term_id → weight (> 0 for active terms)

    bool empty() const { return terms.empty(); }
    size_t size() const { return terms.size(); }
};

/// F40 §4.2 sparse_vec on-disk serialization (the F34 children.sparse_vec BLOB).
///
/// Wire format (little-endian, packed; matches the §4.2 sketch):
///   [uint16 num_terms] then num_terms × ([uint16 term_id][float weight])
/// Size = 2 + num_terms × 6 bytes (K=100 → 602 bytes). An empty vector
/// serializes to the 2-byte header {num_terms=0}; the caller, however, should
/// store SQL NULL for a "no sparse signal" chunk and clear the F09
/// flags_ext has_sparse_vec bit — an all-zero BLOB is reserved for the rare
/// "deserialized-but-empty" round-trip, not the dead-chunk sentinel.
///
/// term_id is stored as uint16 per §4.2. The real BGE-M3 vocab is 250K (needs
/// ~18 bits) — a term_id ≥ 65536 is rejected by Serialize with
/// CX_ERR_F40_SPARSE_SERIALIZE_FAILED (the standalone stub keeps ids in-range;
/// the wider real-vocab encoding is Phase 2, see PHASE2_BACKLOG / F40 §4.2 note).
std::vector<uint8_t> SerializeSparseVec(const SparseVector& vec, bool* ok = nullptr);

/// Parse a sparse_vec BLOB produced by SerializeSparseVec. Returns the vector on
/// success. A malformed BLOB (truncated header / declared num_terms exceeds the
/// available bytes) → CX_ERR_F40_SPARSE_SERIALIZE_FAILED-coded Status. An empty
/// input (size 0) round-trips to an empty SparseVector (defensive: a NULL column
/// read as an empty buffer is treated as "no sparse vector", not an error).
Result<SparseVector> DeserializeSparseVec(const std::vector<uint8_t>& blob);

/// Overload over a raw pointer/length (zero-copy read straight from the SQLite
/// column without an intermediate vector copy).
Result<SparseVector> DeserializeSparseVec(const uint8_t* data, size_t len);

/// Byte size SerializeSparseVec(vec) would produce, without serializing
/// (2 + terms × 6). Lets callers size buffers / enforce the §6.5
/// serialize-truncation guard before paying for the bytes.
size_t SerializedSparseVecSize(const SparseVector& vec);

}  // namespace cortrix::retrieval
