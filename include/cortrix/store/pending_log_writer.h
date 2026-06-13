#pragma once
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "cortrix/common/result.h"
#include "cortrix/common/status.h"
#include "cortrix/store/group_commit_writer.h"
#include "cortrix/store/pending_entry.h"

namespace cortrix::store {

/// Aggregate counters over the records currently in pending.wal (design § 2.1
/// WalStats). `pending` is the count of txns that have a PENDING record but no
/// COMMITTED/ROLLBACK yet — the operational backlog gauge.
struct WalStats {
    uint64_t pending = 0;
    uint64_t committed = 0;
    uint64_t rollback = 0;
};

/// Pending Write Log file (design § 3.1/§3.2) and the real IWalSink behind the
/// shared GroupCommitWriter.
///
/// PendingLogWriter owns the `pending.wal` file descriptor and is the durable
/// backend: GroupCommitWriter coalesces concurrent Append() calls into batches
/// and drives exactly one WriteBatch()+Sync() per batch against `this`. So a
/// PENDING/COMMITTED/ROLLBACK record is crash-recoverable once Append() returns
/// Ok (its batch reached fdatasync). The group-commit machinery is reused, not
/// reimplemented (design § 2.2 / CODING_CONVENTIONS — shared GroupCommitWriter).
///
/// Thread-safe: Append/AppendBatch are safe to call concurrently; ReadAll and
/// Compact take the file mutex and must not race a live Append (the coordinator
/// calls Compact while holding its own txn lock, i.e. no in-flight appends).
class PendingLogWriter : public IWalSink {
public:
    /// File layout for the 32-byte header (design § 3.2).
    static constexpr char kMagic[4] = {'P', 'W', 'L', 'G'};
    static constexpr uint16_t kVersion = 1;
    static constexpr size_t kHeaderSize = 32;

    /// Open (creating if absent) the pending.wal at `path`. On a fresh file the
    /// 32-byte header is written and synced; on an existing file the header is
    /// validated (magic/version/header_crc). Group commit coalesces up to
    /// `max_batch` records or `timeout_ms` latency per fsync.
    static Result<std::unique_ptr<PendingLogWriter>> Open(const std::string& path,
                                                          int max_batch, int timeout_ms);

    ~PendingLogWriter() override;

    PendingLogWriter(const PendingLogWriter&) = delete;
    PendingLogWriter& operator=(const PendingLogWriter&) = delete;

    /// Append one record and block until its batch is durable. Returns the
    /// batch's durability Status (CX_ERR_PWL_WRITE_FAILED on write/sync error).
    Status Append(const PendingEntry& entry);

    /// Testing seam: force the next `n` appended entries (via Append() or
    /// AppendBatch()) to fail with a CX_ERR_PWL_WRITE_FAILED status without
    /// touching the file, to exercise the caller's transient-failure retry path
    /// (F25 Q1-B). `n == 0` disables it. Not part of the production write path;
    /// thread-safe.
    void SetAppendFailures(int n) { append_failures_.store(n, std::memory_order_relaxed); }

    /// Append several records in one shot; each rides the group-commit batches
    /// and the call returns once all are durable. On the first failure the
    /// failing Status is returned (later records may or may not have synced —
    /// recovery reconciles via the three-way consistency check).
    Status AppendBatch(const std::vector<PendingEntry>& entries);

    /// Read every record in EOF order (design § 4.4 — does not trust any header
    /// count). A trailing record that fails its CRC or is short truncates the
    /// scan: all records up to the corruption are returned and the file is
    /// truncated to the last valid record (degraded recovery, design § 5).
    Result<std::vector<PendingEntry>> ReadAll();

    /// Rewrite pending.wal keeping only records for txns that are NOT finalized
    /// (no COMMITTED/ROLLBACK seen). Finalized txns — and their now-redundant
    /// PENDING records — are dropped. Written atomically via a temp file +
    /// rename, then fsync. Safe to call when no Append is in flight.
    Status Compact();

    /// Snapshot of pending/committed/rollback record counts (design § 2.1).
    /// Reads the file; O(file size).
    Result<WalStats> GetStats();

    /// Current on-disk size of pending.wal in bytes (header + all durable
    /// records), as last extended by WriteBatch. O(1) — reads a cached counter
    /// updated under file_mu_, never touches the filesystem. Backs
    /// WriteCoordinator::GetPwlSizeBytes() for F05 memory-budget tracking
    /// (design § 2.1 v1.0.1). Records still buffered in the group-commit queue
    /// are not yet counted (they have no file bytes until their batch writes).
    size_t SizeBytes() const;

    /// Flush queued records and stop the background commit thread. Idempotent;
    /// also run by the destructor.
    void Shutdown();

    // --- IWalSink (driven by the GroupCommitWriter; do not call directly) ---
    Status WriteBatch(const std::vector<std::vector<uint8_t>>& entries) override;
    Status Sync() override;

private:
    PendingLogWriter(int fd, std::string path, size_t initial_size,
                     int max_batch, int timeout_ms);

    // Read the whole file into memory (header + records); helper for ReadAll /
    // Compact / GetStats. Caller holds file_mu_.
    Result<std::vector<uint8_t>> ReadFileLocked();

    // Parse records out of an in-memory file image (skips the 32B header),
    // stopping at the first corrupt/short record. `valid_end` receives the byte
    // offset just past the last good record (for tail truncation).
    static std::vector<PendingEntry> ScanRecords(const std::vector<uint8_t>& image,
                                                 size_t* valid_end);

    /// Consume one injected append fault if armed (SetAppendFailures seam).
    bool TryConsumeInjectedFault();

    int fd_;                  ///< owned; pending.wal, opened O_RDWR
    std::string path_;
    std::mutex file_mu_;      ///< serializes WriteBatch/Sync/ReadAll/Compact on fd_
    std::atomic<size_t> file_size_;  ///< cached on-disk byte count (SizeBytes, O(1))
    std::atomic<int> append_failures_{0};  ///< test fault injection (SetAppendFailures)
    std::unique_ptr<GroupCommitWriter> committer_;  ///< sink = this
};

}  // namespace cortrix::store
