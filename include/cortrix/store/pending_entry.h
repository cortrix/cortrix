#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "cortrix/common/result.h"
#include "cortrix/common/status.h"
#include "cortrix/common/types.h"

namespace cortrix::store {

/// One transaction record in the Pending Write Log (pending.wal).
///
/// F25 Write Coordinator persists a tiny state machine per document write:
/// a PENDING record is appended before the three-way store write begins, then
/// a COMMITTED or ROLLBACK record finalizes it (keyed by txn_id). Crash recovery
/// replays these records to decide which transactions to clean up.
///
/// On-disk frame (little-endian, F25 design § 3.1) — `checksum` covers every
/// field from `entry_type` through `block_ids` (i.e. everything except the
/// leading `entry_size` and the trailing `checksum` itself):
///
///   entry_size   u32   bytes that follow this field (type..checksum)
///   entry_type   u8    1=PENDING, 2=COMMITTED, 3=ROLLBACK
///   txn_id       u64
///   timestamp    i64   Unix milliseconds
///   doc_id_len   u16
///   doc_id       bytes (doc_id_len)
///   block_count  u32
///   block_ids    u64 * block_count   (only meaningful for PENDING)
///   has_blob     u8    0/1   (PENDING only — v1.0.2 D3.5 #1; gates Recover blob check;
///                      required: a PENDING frame without it is corrupt)
///   scope        u8    0 = document, 1 = blocks-only (PENDING only; the first OPTIONAL
///                      trailing field: records written before it existed end at
///                      has_blob and read as document; an unknown value also reads
///                      as document, never as corrupt, because a rejected record
///                      truncates the log at that point)
///   checksum     u32   CRC32 over the whole body (entry_type..last body byte)
///
/// The reader never requires the parse position to reach the body's end, so a
/// binary that predates `scope` reads a frame carrying it exactly as before (the
/// CRC covers the stored body either way). COMMITTED / ROLLBACK records carry an
/// empty doc_id, no block_ids, no has_blob and no scope byte; they reference the
/// originating PENDING purely by txn_id (design § 3.1) and end right after
/// block_count.
struct PendingEntry {
    enum class State : uint8_t {
        kPending = 1,
        kCommitted = 2,
        kRollback = 3,
    };

    /// What the transaction may have written beyond the blocks it lists. Rollback
    /// compensation — explicit or at recovery — is limited to this scope. Only
    /// the ingest transaction writes the document's parents and its
    /// document-level full-text row; a repair (enrichment backfill, document
    /// summary) writes only the blocks it lists and their vector points.
    enum class Scope : uint8_t {
        kDocument = 0,    ///< ingest: the listed blocks, the parents, the full-text row
        kBlocksOnly = 1,  ///< repairs: only the listed blocks and their vector points
    };

    State state = State::kPending;
    uint64_t txn_id = 0;
    int64_t timestamp_ms = 0;  ///< Unix milliseconds
    std::string doc_id;
    std::vector<BlockId> block_ids;
    bool has_blob = false;  ///< v1.0.2 (D3.5 #1): did this txn write a blob? Only
                            ///< meaningful for PENDING; gates the Recover three-way
                            ///< blob check (§4.4). On-disk only for PENDING records.
    Scope scope = Scope::kDocument;  ///< PENDING only; optional trailing byte, see above.

    /// Serialize into the on-disk frame above (length-prefixed, CRC-suffixed).
    /// The returned buffer is one self-describing, caller-framed WAL record —
    /// exactly what IWalSink::WriteBatch / GroupCommitWriter::Submit expect.
    std::vector<uint8_t> Serialize() const;

    /// Parse one frame previously produced by Serialize(). `bytes` must contain
    /// exactly one record (no trailing data). Verifies the CRC and the internal
    /// length fields; on any mismatch returns CX_ERR_PWL_CORRUPTED.
    static Result<PendingEntry> Deserialize(const std::vector<uint8_t>& bytes);

    /// Parse one frame from a byte span, advancing `offset` past it on success.
    /// Used by the EOF-order recovery scan, which reads back-to-back records
    /// from a single buffer. On a short/corrupt record `offset` is left
    /// unchanged and CX_ERR_PWL_CORRUPTED is returned (caller truncates the
    /// tail — design § 5 "on CRC failure, truncate to the last valid entry").
    static Result<PendingEntry> DeserializeFrom(const uint8_t* data, size_t size,
                                                size_t* offset);
};

}  // namespace cortrix::store
