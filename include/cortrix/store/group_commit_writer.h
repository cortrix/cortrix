#pragma once
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

#include "cortrix/common/status.h"

namespace cortrix::store {

/// Durable backend abstraction for GroupCommitWriter.
///
/// A batch becomes durable only after Sync() returns Ok. GroupCommitWriter
/// guarantees exactly one WriteBatch()+Sync() pair per coalesced batch, so an
/// implementation amortizes its fsync cost across all writers in the batch.
/// Implemented by the PWL writer and the P-HNSW WAL backend.
class IWalSink {
public:
    virtual ~IWalSink() = default;

    /// Append a batch of already-framed records (each entry is one opaque,
    /// caller-framed WAL record, e.g. including its own CRC). Not yet durable.
    virtual Status WriteBatch(const std::vector<std::vector<uint8_t>>& entries) = 0;

    /// Flush previously written records to stable storage (fdatasync).
    /// Returns once the data survives a power loss.
    virtual Status Sync() = 0;
};

/// Runtime counters for observability (wired to the observability spec metrics by
/// the consuming feature). Snapshot via GroupCommitWriter::GetStats().
struct GroupCommitStats {
    uint64_t submitted = 0;  ///< total Submit() calls accepted
    uint64_t batches   = 0;  ///< total WriteBatch()+Sync() cycles
    uint64_t synced    = 0;  ///< entries that reached durable storage
    uint64_t failed    = 0;  ///< entries whose batch failed write or sync
    uint64_t pending   = 0;  ///< entries queued but not yet flushed (live)
};

/// Group-commit WAL writer (shared scaffolding).
///
/// Many threads call Submit() concurrently; a single background thread coalesces
/// the queued entries into batches — bounded by `max_batch` entries or
/// `timeout_ms` latency, whichever comes first — and performs one
/// WriteBatch()+Sync() per batch. Each Submit() future resolves with the
/// durability Status of its batch: an Ok future therefore implies Sync()
/// succeeded, i.e. the entry is crash-recoverable (durability handoff for the
/// WAL recovery path owned by the write coordinator / index).
///
/// Consumed by P-HNSW and the PWL. Non-copyable; the sink is borrowed and
/// must outlive the writer.
class GroupCommitWriter {
public:
    /// @param sink        durable backend, not owned, must outlive this writer
    /// @param max_batch   flush once this many entries are queued (clamped to >=1)
    /// @param timeout_ms  flush a partial batch after this latency (clamped to >=0)
    GroupCommitWriter(IWalSink* sink, int max_batch, int timeout_ms);
    ~GroupCommitWriter();

    GroupCommitWriter(const GroupCommitWriter&) = delete;
    GroupCommitWriter& operator=(const GroupCommitWriter&) = delete;

    /// Queue one framed entry for durable commit. The returned future resolves
    /// with Status::Ok() once the entry's batch is synced, or the propagated
    /// write/sync error. After Shutdown(), resolves immediately with Unavailable.
    std::future<Status> Submit(std::vector<uint8_t> entry_bytes);

    /// Flush all queued entries, then stop the background thread. Idempotent;
    /// all outstanding futures are resolved before it returns. Also called by
    /// the destructor.
    void Shutdown();

    /// Thread-safe snapshot of the counters (pending reflects the live queue).
    GroupCommitStats GetStats() const;

private:
    struct Pending {
        std::vector<uint8_t> bytes;
        std::promise<Status> done;
    };

    void FlushLoop();
    void FlushBatch(std::vector<Pending>& batch);
    // Last-resort waiter resolution if FlushBatch throws unexpectedly (R2-M3):
    // resolve any still-pending promise so no Submit() future hangs forever.
    static void ResolvePendingWaiters(std::vector<Pending>& batch);

    IWalSink* const sink_;
    const std::size_t max_batch_;
    const int timeout_ms_;

    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::vector<Pending> queue_;  // guarded by mu_
    bool shutdown_ = false;       // guarded by mu_
    GroupCommitStats stats_;      // guarded by mu_ (pending filled live in GetStats)

    std::thread worker_;
};

/// CRC32 over `length` bytes of `data`, IEEE 802.3 polynomial (0xEDB88320),
/// table-driven — identical to zlib's crc32 so frames stay tool-compatible.
/// Shared utility for WAL record framing (PWL / index integrity checks).
/// `data` may be null iff `length == 0`.
std::uint32_t Crc32(const std::uint8_t* data, std::size_t length);

}  // namespace cortrix::store
