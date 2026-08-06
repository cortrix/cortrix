#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "cortrix/common/result.h"
#include "cortrix/common/status.h"
#include "cortrix/store/group_commit_writer.h"  // IWalSink
#include "cortrix/store/phnsw/wal_entry.h"

namespace cortrix::store {

/// Aggregate counters over hnsw.wal (WalStats — distinct from
/// PHnsw's higher-level WalStats in phnsw.h; this one is the file-level view).
struct WalWriterStats {
    uint64_t entry_count = 0;       ///< records currently in the WAL
    uint64_t committed_lsn = 0;     ///< last committed LSN (== entry_count after a clean flush)
    std::size_t file_size_bytes = 0;
};

/// P-HNSW write-ahead log file (hnsw.wal, design) and the real
/// IWalSink behind the shared GroupCommitWriter.
///
/// S2 (this Story) builds the file layer: the 32-byte header, framed record
/// append (write + fdatasync), full EOF-order read for recovery, and truncate.
/// S3 layers a GroupCommitWriter on top so concurrent AddPoint calls coalesce
/// into one fsync per batch; because WalWriter already implements
/// IWalSink::WriteBatch/Sync, S3 only has to construct the committer with
/// `sink = this`. A record is crash-recoverable once its Append() returns Ok
/// (its bytes reached fdatasync) — same durability contract as the PWL.
///
/// On-disk header (32 bytes, design, little-endian):
///   magic         "PWAL" (4B)
///   version       u16
///   dim           u32
///   entry_count   u64
///   committed_lsn u64
///   header_crc    u32   CRC32 over magic..committed_lsn
///   reserved      u16
///
/// Thread-safe: Append/AppendBatch/WriteBatch/Sync/ReadAll/Truncate serialize on
/// an internal file mutex.
class WalWriter : public IWalSink {
public:
    static constexpr char kMagic[4] = {'P', 'W', 'A', 'L'};
    static constexpr uint16_t kVersion = 1;
    static constexpr std::size_t kHeaderSize = 32;

    /// Open (creating if absent) the hnsw.wal at `wal_path` for vectors of
    /// `dim`. A fresh file gets the 32-byte header written + synced; an existing
    /// file has its header validated (magic/version/header_crc) and its declared
    /// `dim` must equal `dim` (else CX_ERR_PHNSW_WAL_CORRUPTED). On a header_crc
    /// failure the count fields are not trusted — ReadAll re-derives them by
    /// scanning (design).
    static Result<std::unique_ptr<WalWriter>> Open(const std::string& wal_path, int dim);

    ~WalWriter() override;

    WalWriter(const WalWriter&) = delete;
    WalWriter& operator=(const WalWriter&) = delete;

    /// Append one record and fdatasync. Returns CX_ERR_PHNSW_WAL_WRITE_FAILED on
    /// a write error, CX_ERR_PHNSW_WAL_SYNC_FAILED on an fsync error. The record
    /// bytes are produced by WalEntry::Serialize*.
    Status Append(const std::vector<uint8_t>& record);

    /// Append several records in one write + one fdatasync (the S2 batch path;
    /// S3's group commit calls WriteBatch/Sync instead).
    Status AppendBatch(const std::vector<std::vector<uint8_t>>& records);

    /// Read every record in EOF order (design — does NOT trust the header
    /// entry_count). A trailing record that fails its CRC or is short truncates
    /// the scan: all records up to the corruption are returned and the file is
    /// truncated to the last valid record (degraded recovery, design).
    /// `expected_dim` cross-checks INSERT vectors (0 = accept declared dim).
    Result<std::vector<WalEntry>> ReadAll(int expected_dim = 0);

    /// Clear all records, keeping the header. entry_count goes to 0 but
    /// committed_lsn is PRESERVED — it is a monotonic absolute counter across the
    /// WAL's lifetime, so post-truncate appends keep ascending LSNs and recovery
    /// can still tell which records a snapshot already incorporates (design
    /// LSN filtering). Called by Snapshot (S4) once a snapshot captures the
    /// state the WAL described. Atomic: header rewritten + file truncated + fsync.
    Status Truncate();

    /// File-level stats (entry_count + monotonic committed_lsn from the header,
    /// plus the on-disk file size).
    Result<WalWriterStats> GetStats();

    int dim() const { return dim_; }

    /// Monotonic absolute LSN of the last appended record (survives Truncate).
    /// The first record currently in the file therefore has LSN
    /// `committed_lsn() - entry_count() + 1`; recovery uses this base to skip
    /// records a snapshot already covers. Thread-safe.
    uint64_t committed_lsn();
    uint64_t entry_count();

    // --- IWalSink (driven by the GroupCommitWriter in S3; ok to call in S2) ---
    Status WriteBatch(const std::vector<std::vector<uint8_t>>& entries) override;
    Status Sync() override;

private:
    WalWriter(int fd, std::string path, int dim, uint64_t entry_count,
              uint64_t committed_lsn);

    // Append `records` to the file (no fsync) and bump the in-memory counters
    // (entry_count_ and the monotonic committed_lsn_). Caller holds file_mu_.
    Status WriteRecordsLocked(const std::vector<std::vector<uint8_t>>& records);
    // Persist the current entry_count/committed_lsn into the on-disk header.
    // Caller holds file_mu_.
    Status RewriteHeaderLocked();
    // fdatasync the fd. Caller holds file_mu_.
    Status SyncLocked();
    // Read the whole file into memory. Caller holds file_mu_.
    Result<std::vector<uint8_t>> ReadFileLocked();

    int fd_;                   ///< owned; hnsw.wal, opened O_RDWR | O_CREAT
    std::string path_;
    int dim_;
    std::mutex file_mu_;       ///< serializes all fd_ access
    uint64_t entry_count_;     ///< guarded by file_mu_; records currently in file
    uint64_t committed_lsn_;   ///< guarded by file_mu_; monotonic, survives Truncate
    bool header_trusted_;      ///< false if header_crc failed on Open (counts re-derived by ReadAll)
};

}  // namespace cortrix::store
