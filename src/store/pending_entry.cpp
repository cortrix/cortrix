#include "cortrix/store/pending_entry.h"

#include <cstring>

#include "cortrix/store/group_commit_writer.h"  // Crc32
#include "cortrix/store/pwl_errors.h"

namespace cortrix::store {

namespace {

// Fixed on-disk layout offsets (design § 3.1). The leading entry_size (u32) is
// followed by the CRC-covered body; the trailing u32 is the CRC itself.
constexpr size_t kSizePrefixLen = 4;   // entry_size (u32), not CRC-covered
constexpr size_t kChecksumLen = 4;     // trailing CRC32 (u32)

// Body fixed-width prefix before the variable doc_id: type(1)+txn(8)+ts(8)+len(2).
constexpr size_t kBodyHeadLen = 1 + 8 + 8 + 2;
// block_count (u32) sits after doc_id; block_ids (u64 each) follow it.
constexpr size_t kBlockCountLen = 4;

// Little-endian append helpers. We frame everything LE so the WAL is portable
// and tool-readable regardless of host endianness (CRC32 matches zlib).
void PutU8(std::vector<uint8_t>& out, uint8_t v) { out.push_back(v); }

void PutU16(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}

void PutU32(std::vector<uint8_t>& out, uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
}

void PutU64(std::vector<uint8_t>& out, uint64_t v) {
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
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

}  // namespace

std::vector<uint8_t> PendingEntry::Serialize() const {
    // doc_id and block_ids are only carried by PENDING records; COMMITTED /
    // ROLLBACK reference the originating txn purely by txn_id (design § 3.1).
    const bool is_pending = (state == State::kPending);
    const std::string& did = is_pending ? doc_id : std::string();
    const size_t block_count = is_pending ? block_ids.size() : 0;

    // Body = type..block_ids, i.e. exactly the bytes the CRC covers.
    std::vector<uint8_t> body;
    body.reserve(kBodyHeadLen + did.size() + kBlockCountLen + block_count * 8);
    PutU8(body, static_cast<uint8_t>(state));
    PutU64(body, txn_id);
    PutU64(body, static_cast<uint64_t>(timestamp_ms));
    PutU16(body, static_cast<uint16_t>(did.size()));
    body.insert(body.end(), did.begin(), did.end());
    PutU32(body, static_cast<uint32_t>(block_count));
    if (is_pending) {
        for (BlockId b : block_ids) PutU64(body, static_cast<uint64_t>(b));
        // has_blob (1B) trails block_ids, PENDING only (§3.1 v1.0.2). COMMITTED/
        // ROLLBACK frames stop at block_count, keeping their fixed 31-byte size.
        PutU8(body, has_blob ? 1 : 0);
    }

    const uint32_t crc = Crc32(body.data(), body.size());

    std::vector<uint8_t> frame;
    frame.reserve(kSizePrefixLen + body.size() + kChecksumLen);
    // entry_size counts the bytes that follow it: body + checksum.
    PutU32(frame, static_cast<uint32_t>(body.size() + kChecksumLen));
    frame.insert(frame.end(), body.begin(), body.end());
    PutU32(frame, crc);
    return frame;
}

Result<PendingEntry> PendingEntry::DeserializeFrom(const uint8_t* data, size_t size,
                                                   size_t* offset) {
    const size_t start = *offset;
    if (size < start + kSizePrefixLen) {
        return PwlStatus(StatusCode::kInternal, pwl_errors::kCorrupted,
                         "truncated entry_size prefix");
    }
    const uint32_t follow = GetU32(data + start);  // bytes after entry_size
    if (follow < kChecksumLen) {
        return PwlStatus(StatusCode::kInternal, pwl_errors::kCorrupted,
                         "entry_size smaller than checksum");
    }
    const size_t frame_end = start + kSizePrefixLen + follow;
    if (frame_end > size) {
        return PwlStatus(StatusCode::kInternal, pwl_errors::kCorrupted,
                         "entry_size overruns buffer");
    }

    const size_t body_begin = start + kSizePrefixLen;
    const size_t body_len = follow - kChecksumLen;
    // CRC stored immediately after the body; verify before trusting any field.
    const uint32_t stored_crc = GetU32(data + body_begin + body_len);
    const uint32_t actual_crc = Crc32(data + body_begin, body_len);
    if (stored_crc != actual_crc) {
        return PwlStatus(StatusCode::kInternal, pwl_errors::kCorrupted, "CRC mismatch");
    }

    // Body is CRC-clean; parse it. Bounds were validated above, but each
    // variable-length read is still range-checked so a self-consistent-CRC yet
    // internally-inconsistent frame can't read past the body.
    const uint8_t* p = data + body_begin;
    size_t pos = 0;
    if (body_len < kBodyHeadLen) {
        return PwlStatus(StatusCode::kInternal, pwl_errors::kCorrupted, "body shorter than header");
    }

    PendingEntry e;
    const uint8_t raw_state = p[pos];
    pos += 1;
    if (raw_state < static_cast<uint8_t>(State::kPending) ||
        raw_state > static_cast<uint8_t>(State::kRollback)) {
        return PwlStatus(StatusCode::kInternal, pwl_errors::kCorrupted, "unknown entry_type");
    }
    e.state = static_cast<State>(raw_state);
    e.txn_id = GetU64(p + pos);
    pos += 8;
    e.timestamp_ms = static_cast<int64_t>(GetU64(p + pos));
    pos += 8;
    const uint16_t doc_id_len = GetU16(p + pos);
    pos += 2;

    if (body_len < pos + doc_id_len + kBlockCountLen) {
        return PwlStatus(StatusCode::kInternal, pwl_errors::kCorrupted, "doc_id overruns body");
    }
    e.doc_id.assign(reinterpret_cast<const char*>(p + pos), doc_id_len);
    pos += doc_id_len;

    const uint32_t block_count = GetU32(p + pos);
    pos += kBlockCountLen;
    if (body_len < pos + static_cast<size_t>(block_count) * 8) {
        return PwlStatus(StatusCode::kInternal, pwl_errors::kCorrupted, "block_ids overrun body");
    }
    e.block_ids.reserve(block_count);
    for (uint32_t i = 0; i < block_count; ++i) {
        e.block_ids.push_back(static_cast<BlockId>(GetU64(p + pos)));
        pos += 8;
    }

    // has_blob (1B) — PENDING only (§3.1 v1.0.2). COMMITTED/ROLLBACK frames end at
    // block_count (block_count=0), so only PENDING carries the byte; the others
    // keep e.has_blob at its default false.
    if (e.state == State::kPending) {
        if (body_len < pos + 1) {
            return PwlStatus(StatusCode::kInternal, pwl_errors::kCorrupted,
                             "has_blob overruns body");
        }
        e.has_blob = (p[pos] != 0);
        pos += 1;
    }

    *offset = frame_end;
    return e;
}

Result<PendingEntry> PendingEntry::Deserialize(const std::vector<uint8_t>& bytes) {
    size_t offset = 0;
    Result<PendingEntry> r = DeserializeFrom(bytes.data(), bytes.size(), &offset);
    if (!r.ok()) return r;
    if (offset != bytes.size()) {
        return PwlStatus(StatusCode::kInternal, pwl_errors::kCorrupted,
                         "trailing bytes after single entry");
    }
    return r;
}

}  // namespace cortrix::store
