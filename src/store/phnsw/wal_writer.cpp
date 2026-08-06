#include <cstdint>
#include "cortrix/store/phnsw/wal_writer.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <utility>

#include "cortrix/store/phnsw_errors.h"

namespace cortrix::store {

namespace {

// Flush `fd` to stable storage. The WAL durability contract (group_commit_writer
// .h IWalSink::Sync) is "survives a power loss", so on macOS we use F_FULLFSYNC
// — plain fsync there does not flush the drive's write cache — and fdatasync on
// Linux. Returns 0 on success, -1 with errno set. (Identical policy to the write coordinator's PWL
// DurableSync — same project-wide durability guarantee.)
int DurableSync(int fd) {
#if defined(__APPLE__)
    return ::fcntl(fd, F_FULLFSYNC) == -1 ? -1 : 0;
#elif defined(_POSIX_SYNCHRONIZED_IO) && _POSIX_SYNCHRONIZED_IO > 0
    return ::fdatasync(fd);
#else
    return ::fsync(fd);
#endif
}

// --- little-endian header field helpers (32B header, design) ---
void PutU16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
}
void PutU32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>((v >> (8 * i)) & 0xFF);
}
void PutU64(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>((v >> (8 * i)) & 0xFF);
}
uint16_t GetU16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}
uint32_t GetU32(const uint8_t* p) {
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(p[i]) << (8 * i);
    return v;
}
uint64_t GetU64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(p[i]) << (8 * i);
    return v;
}

// Header byte layout (design):
//   [0,4)   magic "PWAL"
//   [4,6)   version u16
//   [6,10)  dim u32
//   [10,18) entry_count u64
//   [18,26) committed_lsn u64
//   [26,28) reserved u16  (zero)
//   [28,32) header_crc u32  (CRC32 over [0,28))
constexpr size_t kHdrMagic = 0;
constexpr size_t kHdrVersion = 4;
constexpr size_t kHdrDim = 6;
constexpr size_t kHdrEntryCount = 10;
constexpr size_t kHdrCommittedLsn = 18;
constexpr size_t kHdrCrcCovered = 28;  // bytes [0,28) are CRC-covered
constexpr size_t kHdrCrc = 28;

// Build the 32B header image for the given dim / counts.
std::vector<uint8_t> MakeHeader(int dim, uint64_t entry_count, uint64_t committed_lsn) {
    std::vector<uint8_t> h(WalWriter::kHeaderSize, 0);
    std::memcpy(h.data() + kHdrMagic, WalWriter::kMagic, 4);
    PutU16(h.data() + kHdrVersion, WalWriter::kVersion);
    PutU32(h.data() + kHdrDim, static_cast<uint32_t>(dim));
    PutU64(h.data() + kHdrEntryCount, entry_count);
    PutU64(h.data() + kHdrCommittedLsn, committed_lsn);
    // bytes [26,28) reserved == 0
    const uint32_t crc = Crc32(h.data(), kHdrCrcCovered);
    PutU32(h.data() + kHdrCrc, crc);
    return h;
}

// Write exactly `len` bytes at file offset `off`, looping over short writes.
Status PwriteAll(int fd, const uint8_t* buf, size_t len, off_t off, const char* what) {
    size_t done = 0;
    while (done < len) {
        ssize_t n = ::pwrite(fd, buf + done, len - done, off + static_cast<off_t>(done));
        if (n < 0) {
            if (errno == EINTR) continue;
            return PhnswStatus(StatusCode::kUnavailable, phnsw_errors::kWalWriteFailed,
                               std::string(what) + " pwrite: " + std::strerror(errno));
        }
        done += static_cast<size_t>(n);
    }
    return Status::Ok();
}

}  // namespace

WalWriter::WalWriter(int fd, std::string path, int dim, uint64_t entry_count,
                     uint64_t committed_lsn)
    : fd_(fd),
      path_(std::move(path)),
      dim_(dim),
      entry_count_(entry_count),
      committed_lsn_(committed_lsn),
      header_trusted_(true) {}

WalWriter::~WalWriter() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

Result<std::unique_ptr<WalWriter>> WalWriter::Open(const std::string& wal_path, int dim) {
    if (dim <= 0) {
        return PhnswStatus(StatusCode::kInvalidArgument, phnsw_errors::kVectorDimMismatch,
                           "WalWriter::Open: dim must be > 0");
    }
    int fd = ::open(wal_path.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
        return PhnswStatus(StatusCode::kUnavailable, phnsw_errors::kWalWriteFailed,
                           "WalWriter::Open: open " + wal_path + ": " + std::strerror(errno));
    }

    // Determine current file size.
    off_t end = ::lseek(fd, 0, SEEK_END);
    if (end < 0) {
        ::close(fd);
        return PhnswStatus(StatusCode::kUnavailable, phnsw_errors::kWalWriteFailed,
                           "WalWriter::Open: lseek: " + std::string(std::strerror(errno)));
    }

    bool header_trusted = true;
    uint64_t entry_count = 0;
    uint64_t committed_lsn = 0;

    if (end == 0) {
        // Fresh file: write + sync a header for an empty WAL.
        std::vector<uint8_t> hdr = MakeHeader(dim, /*entry_count=*/0, /*committed_lsn=*/0);
        Status w = PwriteAll(fd, hdr.data(), hdr.size(), 0, "WalWriter::Open header");
        if (!w.ok()) {
            ::close(fd);
            return w;
        }
        if (DurableSync(fd) != 0) {
            ::close(fd);
            return PhnswStatus(StatusCode::kUnavailable, phnsw_errors::kWalSyncFailed,
                               "WalWriter::Open header sync: " + std::string(std::strerror(errno)));
        }
    } else {
        // Existing file: validate the header.
        if (static_cast<size_t>(end) < kHeaderSize) {
            ::close(fd);
            return PhnswStatus(StatusCode::kInvalidArgument, phnsw_errors::kWalCorrupted,
                               "WalWriter::Open: file smaller than header");
        }
        uint8_t hdr[kHeaderSize];
        ssize_t n = ::pread(fd, hdr, kHeaderSize, 0);
        if (n != static_cast<ssize_t>(kHeaderSize)) {
            ::close(fd);
            return PhnswStatus(StatusCode::kUnavailable, phnsw_errors::kWalCorrupted,
                               "WalWriter::Open: short header read");
        }
        if (std::memcmp(hdr + kHdrMagic, kMagic, 4) != 0) {
            ::close(fd);
            return PhnswStatus(StatusCode::kInvalidArgument, phnsw_errors::kWalCorrupted,
                               "WalWriter::Open: bad magic");
        }
        const uint16_t version = GetU16(hdr + kHdrVersion);
        if (version != kVersion) {
            ::close(fd);
            return PhnswStatus(StatusCode::kInvalidArgument, phnsw_errors::kWalCorrupted,
                               "WalWriter::Open: unsupported version");
        }
        const uint32_t file_dim = GetU32(hdr + kHdrDim);
        if (file_dim != static_cast<uint32_t>(dim)) {
            ::close(fd);
            return PhnswStatus(StatusCode::kInvalidArgument, phnsw_errors::kWalCorrupted,
                               "WalWriter::Open: dim mismatch (file vs requested)");
        }
        const uint32_t stored_crc = GetU32(hdr + kHdrCrc);
        const uint32_t actual_crc = Crc32(hdr, kHdrCrcCovered);
        if (stored_crc != actual_crc) {
            // Header CRC failed: do not trust the count fields (design — the
            // entry_count is re-derived by a scan). Keep the file open; ReadAll
            // recomputes counts and can rewrite a clean header. committed_lsn is
            // re-derived as the post-scan entry_count (monotonic base lost, but a
            // fresh sequence is consistent with the surviving records).
            header_trusted = false;
            entry_count = 0;
            committed_lsn = 0;
        } else {
            entry_count = GetU64(hdr + kHdrEntryCount);
            committed_lsn = GetU64(hdr + kHdrCommittedLsn);
        }
    }

    auto writer = std::unique_ptr<WalWriter>(
        new WalWriter(fd, wal_path, dim, entry_count, committed_lsn));
    writer->header_trusted_ = header_trusted;
    return Result<std::unique_ptr<WalWriter>>(std::move(writer));
}

Status WalWriter::WriteRecordsLocked(const std::vector<std::vector<uint8_t>>& records) {
    if (records.empty()) {
        return Status::Ok();
    }
    off_t off = ::lseek(fd_, 0, SEEK_END);
    if (off < 0) {
        return PhnswStatus(StatusCode::kUnavailable, phnsw_errors::kWalWriteFailed,
                           "WalWriter: lseek end: " + std::string(std::strerror(errno)));
    }
    for (const auto& rec : records) {
        Status w = PwriteAll(fd_, rec.data(), rec.size(), off, "WalWriter::WriteRecords");
        if (!w.ok()) {
            return w;
        }
        off += static_cast<off_t>(rec.size());
    }
    entry_count_ += records.size();
    committed_lsn_ += records.size();  // monotonic; never decreases (survives Truncate)
    return Status::Ok();
}

Status WalWriter::RewriteHeaderLocked() {
    std::vector<uint8_t> hdr = MakeHeader(dim_, entry_count_, committed_lsn_);
    Status w = PwriteAll(fd_, hdr.data(), hdr.size(), 0, "WalWriter::RewriteHeader");
    if (w.ok()) {
        header_trusted_ = true;
    }
    return w;
}

Status WalWriter::SyncLocked() {
    if (DurableSync(fd_) != 0) {
        return PhnswStatus(StatusCode::kUnavailable, phnsw_errors::kWalSyncFailed,
                           "WalWriter: fdatasync: " + std::string(std::strerror(errno)));
    }
    return Status::Ok();
}

Status WalWriter::Append(const std::vector<uint8_t>& record) {
    return AppendBatch({record});
}

Status WalWriter::AppendBatch(const std::vector<std::vector<uint8_t>>& records) {
    if (records.empty()) {
        return Status::Ok();
    }
    std::lock_guard<std::mutex> lk(file_mu_);
    Status w = WriteRecordsLocked(records);
    if (!w.ok()) {
        return w;
    }
    // Update the header (entry_count/committed_lsn) then one durable sync covers
    // both the records and the header.
    Status h = RewriteHeaderLocked();
    if (!h.ok()) {
        return h;
    }
    return SyncLocked();
}

Status WalWriter::WriteBatch(const std::vector<std::vector<uint8_t>>& entries) {
    // IWalSink path used by GroupCommitWriter (S3). Write records + header WITHOUT
    // syncing; the committer calls Sync() exactly once afterwards.
    std::lock_guard<std::mutex> lk(file_mu_);
    Status w = WriteRecordsLocked(entries);
    if (!w.ok()) {
        return w;
    }
    return RewriteHeaderLocked();
}

Status WalWriter::Sync() {
    std::lock_guard<std::mutex> lk(file_mu_);
    return SyncLocked();
}

Result<std::vector<uint8_t>> WalWriter::ReadFileLocked() {
    off_t end = ::lseek(fd_, 0, SEEK_END);
    if (end < 0) {
        return PhnswStatus(StatusCode::kUnavailable, phnsw_errors::kWalCorrupted,
                           "WalWriter: lseek end: " + std::string(std::strerror(errno)));
    }
    std::vector<uint8_t> buf(static_cast<size_t>(end));
    size_t done = 0;
    while (done < buf.size()) {
        ssize_t n = ::pread(fd_, buf.data() + done, buf.size() - done,
                            static_cast<off_t>(done));
        if (n < 0) {
            if (errno == EINTR) continue;
            return PhnswStatus(StatusCode::kUnavailable, phnsw_errors::kWalCorrupted,
                               "WalWriter: pread: " + std::string(std::strerror(errno)));
        }
        if (n == 0) break;  // unexpected EOF; return what we have
        done += static_cast<size_t>(n);
    }
    buf.resize(done);
    return Result<std::vector<uint8_t>>(std::move(buf));
}

Result<std::vector<WalEntry>> WalWriter::ReadAll(int expected_dim) {
    std::lock_guard<std::mutex> lk(file_mu_);
    Result<std::vector<uint8_t>> file = ReadFileLocked();
    if (!file.ok()) {
        return file.status();
    }
    const std::vector<uint8_t>& img = file.value();
    std::vector<WalEntry> out;
    if (img.size() < kHeaderSize) {
        // No (or partial) header — treat as empty WAL.
        return Result<std::vector<WalEntry>>(std::move(out));
    }

    const int dim_check = (expected_dim > 0) ? expected_dim : dim_;
    size_t offset = kHeaderSize;
    size_t valid_end = kHeaderSize;
    while (offset < img.size()) {
        Result<WalEntry> e = WalEntry::DeserializeFrom(img.data(), img.size(), &offset, dim_check);
        if (!e.ok()) {
            // Corrupt/short trailing record — stop and truncate the tail (design).
            break;
        }
        out.push_back(std::move(e.value()));
        valid_end = offset;
    }

    // If a tail was corrupt, truncate the file to the last valid record and
    // rewrite the header with the re-derived count so the file is clean.
    const bool had_tail_corruption = (valid_end < img.size());
    if (had_tail_corruption || !header_trusted_) {
        if (had_tail_corruption) {
            if (::ftruncate(fd_, static_cast<off_t>(valid_end)) != 0) {
                return PhnswStatus(StatusCode::kUnavailable, phnsw_errors::kWalCorrupted,
                                   "WalWriter::ReadAll ftruncate tail: " +
                                       std::string(std::strerror(errno)));
            }
        }
        // Re-derive counts. The monotonic base (LSN just before the first record
        // in the file) is preserved; dropped torn-tail records were never durably
        // acked, so un-counting them keeps committed_lsn honest.
        const uint64_t base = committed_lsn_ - entry_count_;
        entry_count_ = out.size();
        committed_lsn_ = base + out.size();
        Status h = RewriteHeaderLocked();
        if (!h.ok()) {
            return h;
        }
        if (DurableSync(fd_) != 0) {
            return PhnswStatus(StatusCode::kUnavailable, phnsw_errors::kWalSyncFailed,
                               "WalWriter::ReadAll sync after repair: " +
                                   std::string(std::strerror(errno)));
        }
    }

    return Result<std::vector<WalEntry>>(std::move(out));
}

Status WalWriter::Truncate() {
    std::lock_guard<std::mutex> lk(file_mu_);
    if (::ftruncate(fd_, static_cast<off_t>(kHeaderSize)) != 0) {
        return PhnswStatus(StatusCode::kUnavailable, phnsw_errors::kWalWriteFailed,
                           "WalWriter::Truncate ftruncate: " + std::string(std::strerror(errno)));
    }
    entry_count_ = 0;
    // committed_lsn_ is PRESERVED (monotonic) — post-truncate appends continue the
    // ascending sequence so recovery's LSN filtering still works (design).
    Status h = RewriteHeaderLocked();
    if (!h.ok()) {
        return h;
    }
    return SyncLocked();
}

Result<WalWriterStats> WalWriter::GetStats() {
    std::lock_guard<std::mutex> lk(file_mu_);
    off_t end = ::lseek(fd_, 0, SEEK_END);
    if (end < 0) {
        return PhnswStatus(StatusCode::kUnavailable, phnsw_errors::kWalCorrupted,
                           "WalWriter::GetStats lseek: " + std::string(std::strerror(errno)));
    }
    WalWriterStats s;
    s.entry_count = entry_count_;
    s.committed_lsn = committed_lsn_;
    s.file_size_bytes = static_cast<std::size_t>(end);
    return Result<WalWriterStats>(s);
}

uint64_t WalWriter::committed_lsn() {
    std::lock_guard<std::mutex> lk(file_mu_);
    return committed_lsn_;
}

uint64_t WalWriter::entry_count() {
    std::lock_guard<std::mutex> lk(file_mu_);
    return entry_count_;
}

}  // namespace cortrix::store
