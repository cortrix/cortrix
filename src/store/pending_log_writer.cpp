#include "cortrix/store/pending_log_writer.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <future>
#include <unordered_map>
#include <unordered_set>

#include "cortrix/store/pwl_errors.h"

namespace cortrix::store {

namespace {

// Flush `fd` to stable storage. The WAL durability contract (group_commit_writer
// .h IWalSink::Sync) is "survives a power loss", so on macOS we use F_FULLFSYNC
// — plain fsync there does not flush the drive's write cache — and fdatasync on
// Linux (data + size, skipping non-essential metadata). Returns 0 on success,
// -1 with errno set otherwise (mirrors fdatasync/fcntl).
int DurableSync(int fd) {
#if defined(__APPLE__)
    return ::fcntl(fd, F_FULLFSYNC) == -1 ? -1 : 0;
#elif defined(_POSIX_SYNCHRONIZED_IO) && _POSIX_SYNCHRONIZED_IO > 0
    return ::fdatasync(fd);
#else
    return ::fsync(fd);
#endif
}

// Little-endian field helpers for the 32B header (kept local; the entry codec
// lives in pending_entry.cpp).
void PutU16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
}
void PutU32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>((v >> (8 * i)) & 0xFF);
}
uint16_t GetU16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}
uint32_t GetU32(const uint8_t* p) {
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(p[i]) << (8 * i);
    return v;
}

// Build the 32B header image: magic(4) version(2) reserved(22) header_crc(4).
// Reserved stays zero in Phase 1 (entry_count / committed_lsn deferred, design
// § 3.2 / TD-PWL-HEADER-FIELDS). header_crc covers magic+version+reserved.
std::vector<uint8_t> MakeHeader() {
    std::vector<uint8_t> h(PendingLogWriter::kHeaderSize, 0);
    std::memcpy(h.data(), PendingLogWriter::kMagic, 4);
    PutU16(h.data() + 4, PendingLogWriter::kVersion);
    // bytes [6, 28) reserved == 0
    const uint32_t crc = Crc32(h.data(), 28);
    PutU32(h.data() + 28, crc);
    return h;
}

// Write exactly `len` bytes at file offset `off`, looping over short writes.
Status PwriteAll(int fd, const uint8_t* buf, size_t len, off_t off) {
    size_t done = 0;
    while (done < len) {
        ssize_t n = ::pwrite(fd, buf + done, len - done, off + static_cast<off_t>(done));
        if (n < 0) {
            if (errno == EINTR) continue;
            return PwlStatus(StatusCode::kUnavailable, pwl_errors::kWriteFailed,
                             std::string("pwrite: ") + std::strerror(errno));
        }
        done += static_cast<size_t>(n);
    }
    return Status::Ok();
}

}  // namespace

PendingLogWriter::PendingLogWriter(int fd, std::string path, size_t initial_size,
                                   int max_batch, int timeout_ms)
    : fd_(fd), path_(std::move(path)), file_size_(initial_size) {
    // Sink is this writer; the committer turns concurrent Append() into batched
    // WriteBatch()+Sync() against our fd.
    committer_ = std::make_unique<GroupCommitWriter>(this, max_batch, timeout_ms);
}

PendingLogWriter::~PendingLogWriter() {
    Shutdown();
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

Result<std::unique_ptr<PendingLogWriter>> PendingLogWriter::Open(const std::string& path,
                                                                int max_batch,
                                                                int timeout_ms) {
    int fd = ::open(path.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
        return PwlStatus(StatusCode::kUnavailable, pwl_errors::kWriteFailed,
                         "open " + path + ": " + std::strerror(errno));
    }

    off_t size = ::lseek(fd, 0, SEEK_END);
    if (size < 0) {
        ::close(fd);
        return PwlStatus(StatusCode::kUnavailable, pwl_errors::kWriteFailed,
                         std::string("lseek: ") + std::strerror(errno));
    }

    if (size == 0) {
        // Fresh file: lay down and sync the header before any record can land.
        std::vector<uint8_t> hdr = MakeHeader();
        Status w = PwriteAll(fd, hdr.data(), hdr.size(), 0);
        if (!w.ok()) {
            ::close(fd);
            return w;
        }
        if (DurableSync(fd) != 0) {
            ::close(fd);
            return PwlStatus(StatusCode::kUnavailable, pwl_errors::kWriteFailed,
                             std::string("sync header: ") + std::strerror(errno));
        }
    } else {
        // Existing file: validate the header so we never append to a foreign or
        // partially-written file.
        if (size < static_cast<off_t>(kHeaderSize)) {
            ::close(fd);
            return PwlStatus(StatusCode::kInternal, pwl_errors::kCorrupted,
                             "pending.wal smaller than header");
        }
        uint8_t hdr[kHeaderSize];
        ssize_t rn = ::pread(fd, hdr, kHeaderSize, 0);
        if (rn != static_cast<ssize_t>(kHeaderSize)) {
            ::close(fd);
            return PwlStatus(StatusCode::kUnavailable, pwl_errors::kWriteFailed,
                             "short header read");
        }
        if (std::memcmp(hdr, kMagic, 4) != 0) {
            ::close(fd);
            return PwlStatus(StatusCode::kInternal, pwl_errors::kCorrupted, "bad WAL magic");
        }
        if (GetU16(hdr + 4) != kVersion) {
            ::close(fd);
            return PwlStatus(StatusCode::kInternal, pwl_errors::kCorrupted, "unsupported WAL version");
        }
        if (GetU32(hdr + 28) != Crc32(hdr, 28)) {
            ::close(fd);
            return PwlStatus(StatusCode::kInternal, pwl_errors::kCorrupted, "header CRC mismatch");
        }
    }

    // Fresh files now hold exactly the header; existing files keep their size.
    // (A corrupt tail, if any, is trimmed lazily by the first ReadAll, which
    // also corrects file_size_.)
    const size_t initial_size =
        (size == 0) ? kHeaderSize : static_cast<size_t>(size);

    // Private ctor — make_unique can't reach it.
    return std::unique_ptr<PendingLogWriter>(
        new PendingLogWriter(fd, path, initial_size, max_batch, timeout_ms));
}

void PendingLogWriter::Shutdown() {
    if (committer_) committer_->Shutdown();
}

// --- IWalSink: append framed records at EOF, then fdatasync. -----------------

Status PendingLogWriter::WriteBatch(const std::vector<std::vector<uint8_t>>& entries) {
    std::lock_guard<std::mutex> lk(file_mu_);
    off_t off = ::lseek(fd_, 0, SEEK_END);
    if (off < 0) {
        return PwlStatus(StatusCode::kUnavailable, pwl_errors::kWriteFailed,
                         std::string("lseek end: ") + std::strerror(errno));
    }
    for (const auto& e : entries) {
        Status w = PwriteAll(fd_, e.data(), e.size(), off);
        if (!w.ok()) return w;
        off += static_cast<off_t>(e.size());
    }
    file_size_.store(static_cast<size_t>(off), std::memory_order_relaxed);
    return Status::Ok();
}

Status PendingLogWriter::Sync() {
    std::lock_guard<std::mutex> lk(file_mu_);
    if (DurableSync(fd_) != 0) {
        return PwlStatus(StatusCode::kUnavailable, pwl_errors::kWriteFailed,
                         std::string("sync: ") + std::strerror(errno));
    }
    return Status::Ok();
}

// --- Append: serialize, submit to group commit, wait for durability. ---------

// Test fault injection (SetAppendFailures): consume one pending fault, if any.
// Production leaves append_failures_ at 0, making this a single relaxed load.
bool PendingLogWriter::TryConsumeInjectedFault() {
    int pending_faults = append_failures_.load(std::memory_order_relaxed);
    while (pending_faults > 0) {
        if (append_failures_.compare_exchange_weak(pending_faults, pending_faults - 1,
                                                   std::memory_order_relaxed)) {
            return true;
        }
        // pending_faults reloaded by compare_exchange_weak on failure; retry.
    }
    return false;
}

Status PendingLogWriter::Append(const PendingEntry& entry) {
    // Fail-fast without writing so the caller's retry path can be exercised
    // deterministically.
    if (TryConsumeInjectedFault()) {
        return PwlStatus(StatusCode::kUnavailable, pwl_errors::kWriteFailed,
                         "injected append failure");
    }
    std::future<Status> fut = committer_->Submit(entry.Serialize());
    return fut.get();
}

Status PendingLogWriter::AppendBatch(const std::vector<PendingEntry>& entries) {
    std::vector<std::future<Status>> futs;
    futs.reserve(entries.size());
    Status first_err = Status::Ok();
    for (const auto& e : entries) {
        // Same injection seam as Append: an injected fault replaces this entry's
        // submit (the rest of the batch still submits and drains below).
        if (TryConsumeInjectedFault()) {
            if (first_err.ok()) {
                first_err = PwlStatus(StatusCode::kUnavailable, pwl_errors::kWriteFailed,
                                      "injected append failure");
            }
            continue;
        }
        futs.push_back(committer_->Submit(e.Serialize()));
    }
    for (auto& f : futs) {
        Status s = f.get();  // drain every future so none is abandoned
        if (!s.ok() && first_err.ok()) first_err = s;
    }
    return first_err;
}

// --- Read / scan. ------------------------------------------------------------

Result<std::vector<uint8_t>> PendingLogWriter::ReadFileLocked() {
    off_t size = ::lseek(fd_, 0, SEEK_END);
    if (size < 0) {
        return PwlStatus(StatusCode::kUnavailable, pwl_errors::kWriteFailed,
                         std::string("lseek: ") + std::strerror(errno));
    }
    std::vector<uint8_t> buf(static_cast<size_t>(size));
    size_t done = 0;
    while (done < buf.size()) {
        ssize_t n = ::pread(fd_, buf.data() + done, buf.size() - done,
                            static_cast<off_t>(done));
        if (n < 0) {
            if (errno == EINTR) continue;
            return PwlStatus(StatusCode::kUnavailable, pwl_errors::kWriteFailed,
                             std::string("pread: ") + std::strerror(errno));
        }
        if (n == 0) break;  // unexpected EOF; stop with what we have
        done += static_cast<size_t>(n);
    }
    buf.resize(done);
    return buf;
}

std::vector<PendingEntry> PendingLogWriter::ScanRecords(const std::vector<uint8_t>& image,
                                                        size_t* valid_end) {
    std::vector<PendingEntry> out;
    size_t off = kHeaderSize <= image.size() ? kHeaderSize : image.size();
    *valid_end = off;
    while (off < image.size()) {
        size_t next = off;
        Result<PendingEntry> r =
            PendingEntry::DeserializeFrom(image.data(), image.size(), &next);
        if (!r.ok()) {
            // Corrupt/short tail — stop; everything before `off` is durable and
            // valid (design § 4.4 / § 5 truncate-to-last-valid).
            break;
        }
        out.push_back(std::move(r.value()));
        off = next;
        *valid_end = off;
    }
    return out;
}

Result<std::vector<PendingEntry>> PendingLogWriter::ReadAll() {
    std::lock_guard<std::mutex> lk(file_mu_);
    Result<std::vector<uint8_t>> img = ReadFileLocked();
    if (!img.ok()) return img.status();

    size_t valid_end = 0;
    std::vector<PendingEntry> entries = ScanRecords(img.value(), &valid_end);

    // If a corrupt tail was found, physically truncate it so subsequent appends
    // start from a clean boundary and we never re-scan the garbage.
    if (valid_end < img.value().size()) {
        if (::ftruncate(fd_, static_cast<off_t>(valid_end)) != 0) {
            return PwlStatus(StatusCode::kUnavailable, pwl_errors::kWriteFailed,
                             std::string("ftruncate tail: ") + std::strerror(errno));
        }
        DurableSync(fd_);
        file_size_.store(valid_end, std::memory_order_relaxed);
    }
    return entries;
}

// --- Compact: keep only non-finalized (PENDING-without-terminal) txns. -------

Status PendingLogWriter::Compact() {
    std::lock_guard<std::mutex> lk(file_mu_);
    Result<std::vector<uint8_t>> img = ReadFileLocked();
    if (!img.ok()) return img.status();

    size_t valid_end = 0;
    std::vector<PendingEntry> entries = ScanRecords(img.value(), &valid_end);

    // A txn is finalized once we've seen a COMMITTED or ROLLBACK for it. Keep
    // only PENDING records whose txn never reached a terminal state.
    std::unordered_set<uint64_t> finalized;
    for (const auto& e : entries) {
        if (e.state == PendingEntry::State::kCommitted ||
            e.state == PendingEntry::State::kRollback) {
            finalized.insert(e.txn_id);
        }
    }

    std::vector<uint8_t> out = MakeHeader();
    for (const auto& e : entries) {
        if (e.state == PendingEntry::State::kPending && finalized.count(e.txn_id) == 0) {
            std::vector<uint8_t> frame = e.Serialize();
            out.insert(out.end(), frame.begin(), frame.end());
        }
    }

    // Atomic replace: write a sibling temp, fsync it, rename over the original,
    // then reopen so fd_ points at the compacted file.
    const std::string tmp = path_ + ".compact.tmp";
    int tfd = ::open(tmp.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (tfd < 0) {
        return PwlStatus(StatusCode::kUnavailable, pwl_errors::kWriteFailed,
                         "open tmp: " + std::string(std::strerror(errno)));
    }
    Status w = PwriteAll(tfd, out.data(), out.size(), 0);
    if (w.ok() && DurableSync(tfd) != 0) {
        w = PwlStatus(StatusCode::kUnavailable, pwl_errors::kWriteFailed,
                      std::string("sync tmp: ") + std::strerror(errno));
    }
    ::close(tfd);
    if (!w.ok()) {
        ::unlink(tmp.c_str());
        return w;
    }
    if (::rename(tmp.c_str(), path_.c_str()) != 0) {
        ::unlink(tmp.c_str());
        return PwlStatus(StatusCode::kUnavailable, pwl_errors::kWriteFailed,
                         std::string("rename: ") + std::strerror(errno));
    }

    // Swap fd_ to the new inode.
    int nfd = ::open(path_.c_str(), O_RDWR, 0644);
    if (nfd < 0) {
        return PwlStatus(StatusCode::kUnavailable, pwl_errors::kWriteFailed,
                         "reopen after compact: " + std::string(std::strerror(errno)));
    }
    ::close(fd_);
    fd_ = nfd;
    file_size_.store(out.size(), std::memory_order_relaxed);
    return Status::Ok();
}

size_t PendingLogWriter::SizeBytes() const {
    return file_size_.load(std::memory_order_relaxed);
}

Result<WalStats> PendingLogWriter::GetStats() {
    std::lock_guard<std::mutex> lk(file_mu_);
    Result<std::vector<uint8_t>> img = ReadFileLocked();
    if (!img.ok()) return img.status();

    size_t valid_end = 0;
    std::vector<PendingEntry> entries = ScanRecords(img.value(), &valid_end);

    // pending = PENDING records whose txn has no terminal record (the live
    // backlog); committed / rollback = count of those terminal records.
    std::unordered_set<uint64_t> finalized;
    WalStats stats;
    for (const auto& e : entries) {
        switch (e.state) {
            case PendingEntry::State::kCommitted:
                ++stats.committed;
                finalized.insert(e.txn_id);
                break;
            case PendingEntry::State::kRollback:
                ++stats.rollback;
                finalized.insert(e.txn_id);
                break;
            case PendingEntry::State::kPending:
                break;
        }
    }
    for (const auto& e : entries) {
        if (e.state == PendingEntry::State::kPending && finalized.count(e.txn_id) == 0) {
            ++stats.pending;
        }
    }
    return stats;
}

}  // namespace cortrix::store
