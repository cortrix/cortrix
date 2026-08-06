#pragma once
#include <cstdint>
#include <vector>

#include "cortrix/common/result.h"
#include "cortrix/common/status.h"

namespace cortrix::store {

/// One mutation record in the P-HNSW write-ahead log (hnsw.wal).
///
/// The index logs every vector mutation before it touches the in-memory graph: an
/// INSERT record carries the full vector, a DELETE record carries only the
/// block_id. Crash recovery (S4) replays these on top of the latest snapshot.
///
/// On-disk frame (little-endian) — `checksum` covers every
/// field from `entry_type` through `vector_data` (i.e. everything except the
/// leading `entry_size` and the trailing `checksum` itself):
///
///   entry_size   u32   bytes that follow this field (type..checksum)
///   entry_type   u8    1=INSERT, 2=DELETE
///   timestamp    i64   Unix milliseconds
///   vector_id    u64   block_id
///   vector_data  f32 * dim   (INSERT only; absent for DELETE)
///   checksum     u32   CRC32 over entry_type..vector_data
///
/// INSERT frame size = 4 + 1 + 8 + 8 + dim*4 + 4  (= 4121 B for dim=1024).
/// DELETE frame size = 4 + 1 + 8 + 8 + 4          (= 25 B).
struct WalEntry {
    enum class Type : uint8_t {
        kInsert = 1,
        kDelete = 2,
    };

    Type type = Type::kInsert;
    int64_t timestamp_ms = 0;     ///< Unix milliseconds
    uint64_t block_id = 0;
    std::vector<float> vector;    ///< dim floats for INSERT; empty for DELETE

    /// Serialize an INSERT record (length-prefixed, CRC-suffixed). `vec` points
    /// to `dim` floats. The returned buffer is one self-describing, caller-framed
    /// WAL record — exactly what IWalSink::WriteBatch / GroupCommitWriter::Submit
    /// expect. `timestamp_ms` defaults to "now" when negative.
    static std::vector<uint8_t> SerializeInsert(uint64_t block_id, const float* vec,
                                                int dim, int64_t timestamp_ms = -1);

    /// Serialize a DELETE record (no vector payload).
    static std::vector<uint8_t> SerializeDelete(uint64_t block_id,
                                                int64_t timestamp_ms = -1);

    /// Parse exactly one frame produced by Serialize* (no trailing data).
    /// Verifies the entry_size and CRC; on any mismatch returns
    /// CX_ERR_PHNSW_WAL_CORRUPTED. `expected_dim` (>0) cross-checks INSERT vector
    /// length; pass 0 to accept whatever the frame declares.
    static Result<WalEntry> Deserialize(const std::vector<uint8_t>& bytes,
                                        int expected_dim = 0);

    /// Parse one frame from a byte span, advancing `*offset` past it on success.
    /// Used by the EOF-order recovery scan (S4) reading back-to-back records. On
    /// a short/corrupt record `*offset` is left unchanged and
    /// CX_ERR_PHNSW_WAL_CORRUPTED is returned (caller truncates the tail — design
    /// "WAL Entry: on CRC failure, truncate to the last valid entry").
    static Result<WalEntry> DeserializeFrom(const uint8_t* data, size_t size,
                                            size_t* offset, int expected_dim = 0);
};

}  // namespace cortrix::store
