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
/// The Write Coordinator persists a tiny state machine per document write:
/// a PENDING record is appended before the three-way store write begins, then
/// a COMMITTED or ROLLBACK record finalizes it (keyed by txn_id). Crash recovery
/// replays these records to decide which transactions to clean up.
///
/// On-disk frame (little-endian) — `checksum` covers every
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
///   has_blob     u8    0/1   (PENDING only — v1.0.2 integration #1; gates Recover blob check)
///   checksum     u32   CRC32 over entry_type..has_blob
///
/// COMMITTED / ROLLBACK records carry an empty doc_id, no block_ids and no
/// has_blob byte; they reference the originating PENDING purely by txn_id
/// (design) and end right after block_count.
struct PendingEntry {
    enum class State : uint8_t {
        kPending = 1,
        kCommitted = 2,
        kRollback = 3,
    };

    State state = State::kPending;
    uint64_t txn_id = 0;
    int64_t timestamp_ms = 0;  ///< Unix milliseconds
    std::string doc_id;
    std::vector<BlockId> block_ids;
    bool has_blob = false;  ///< v1.0.2 (integration #1): did this txn write a blob? Only
                            ///< meaningful for PENDING; gates the Recover three-way
                            ///< blob check. On-disk only for PENDING records.

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
    /// tail — design "on CRC failure, truncate to the last valid entry").
    static Result<PendingEntry> DeserializeFrom(const uint8_t* data, size_t size,
                                                size_t* offset);
};

}  // namespace cortrix::store
