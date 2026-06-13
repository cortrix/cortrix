#include "cortrix/store/phnsw/wal_entry.h"

#include <chrono>
#include <cstring>

#include "cortrix/store/group_commit_writer.h"  // Crc32 (shared)
#include "cortrix/store/phnsw_errors.h"

namespace cortrix::store {

namespace {

// --- little-endian append helpers (match F25 PendingEntry codec style) ---
void PutU8(std::vector<uint8_t>& b, uint8_t v) { b.push_back(v); }

void PutU32(std::vector<uint8_t>& b, uint32_t v) {
    b.push_back(static_cast<uint8_t>(v & 0xFFu));
    b.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
    b.push_back(static_cast<uint8_t>((v >> 16) & 0xFFu));
    b.push_back(static_cast<uint8_t>((v >> 24) & 0xFFu));
}

void PutU64(std::vector<uint8_t>& b, uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        b.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFFu));
    }
}

uint32_t GetU32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t GetU64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= static_cast<uint64_t>(p[i]) << (8 * i);
    }
    return v;
}

int64_t NowMsOrGiven(int64_t timestamp_ms) {
    if (timestamp_ms >= 0) {
        return timestamp_ms;
    }
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// Frame field offsets relative to the start of the entry buffer.
constexpr size_t kEntrySizeBytes = 4;  // u32 entry_size prefix
constexpr size_t kTypeBytes = 1;       // u8 entry_type
constexpr size_t kTimestampBytes = 8;  // i64
constexpr size_t kIdBytes = 8;         // u64 block_id
constexpr size_t kCrcBytes = 4;        // u32 checksum
// "body" = the CRC-covered region (entry_type .. vector_data), i.e. the bytes
// after entry_size and before checksum.
constexpr size_t kFixedBodyBytes = kTypeBytes + kTimestampBytes + kIdBytes;  // 17

// Build a complete frame from the already-assembled CRC-covered body.
std::vector<uint8_t> FrameFromBody(const std::vector<uint8_t>& body) {
    std::vector<uint8_t> out;
    const uint32_t entry_size = static_cast<uint32_t>(body.size() + kCrcBytes);
    out.reserve(kEntrySizeBytes + entry_size);
    PutU32(out, entry_size);
    out.insert(out.end(), body.begin(), body.end());
    const uint32_t crc = Crc32(body.data(), body.size());
    PutU32(out, crc);
    return out;
}

}  // namespace

std::vector<uint8_t> WalEntry::SerializeInsert(uint64_t block_id, const float* vec, int dim,
                                               int64_t timestamp_ms) {
    std::vector<uint8_t> body;
    body.reserve(kFixedBodyBytes + static_cast<size_t>(dim) * sizeof(float));
    PutU8(body, static_cast<uint8_t>(Type::kInsert));
    PutU64(body, static_cast<uint64_t>(NowMsOrGiven(timestamp_ms)));
    PutU64(body, block_id);
    // vector_data: raw little-endian float32[] (x86/arm are LE; matches GetU32
    // reinterpretation on read). Append the raw bytes of each float.
    for (int i = 0; i < dim; ++i) {
        uint32_t bits;
        std::memcpy(&bits, &vec[i], sizeof(bits));
        PutU32(body, bits);
    }
    return FrameFromBody(body);
}

std::vector<uint8_t> WalEntry::SerializeDelete(uint64_t block_id, int64_t timestamp_ms) {
    std::vector<uint8_t> body;
    body.reserve(kFixedBodyBytes);
    PutU8(body, static_cast<uint8_t>(Type::kDelete));
    PutU64(body, static_cast<uint64_t>(NowMsOrGiven(timestamp_ms)));
    PutU64(body, block_id);
    return FrameFromBody(body);
}

Result<WalEntry> WalEntry::Deserialize(const std::vector<uint8_t>& bytes, int expected_dim) {
    size_t offset = 0;
    Result<WalEntry> r = DeserializeFrom(bytes.data(), bytes.size(), &offset, expected_dim);
    if (!r.ok()) {
        return r;
    }
    if (offset != bytes.size()) {
        return PhnswStatus(StatusCode::kInvalidArgument, phnsw_errors::kWalCorrupted,
                           "WalEntry::Deserialize: trailing bytes after one record");
    }
    return r;
}

Result<WalEntry> WalEntry::DeserializeFrom(const uint8_t* data, size_t size, size_t* offset,
                                           int expected_dim) {
    const size_t start = *offset;
    // Need at least the entry_size prefix.
    if (size < start + kEntrySizeBytes) {
        return PhnswStatus(StatusCode::kInvalidArgument, phnsw_errors::kWalCorrupted,
                           "WalEntry: truncated entry_size");
    }
    const uint32_t entry_size = GetU32(data + start);
    // entry_size counts type..checksum; the full frame is prefix + entry_size.
    if (entry_size < kFixedBodyBytes + kCrcBytes) {
        return PhnswStatus(StatusCode::kInvalidArgument, phnsw_errors::kWalCorrupted,
                           "WalEntry: entry_size below minimum");
    }
    const size_t frame_end = start + kEntrySizeBytes + entry_size;
    if (size < frame_end) {
        return PhnswStatus(StatusCode::kInvalidArgument, phnsw_errors::kWalCorrupted,
                           "WalEntry: frame extends past buffer");
    }

    const size_t body_begin = start + kEntrySizeBytes;
    const size_t body_len = entry_size - kCrcBytes;  // CRC-covered region length
    const uint32_t stored_crc = GetU32(data + body_begin + body_len);
    const uint32_t actual_crc = Crc32(data + body_begin, body_len);
    if (stored_crc != actual_crc) {
        return PhnswStatus(StatusCode::kInvalidArgument, phnsw_errors::kWalCorrupted,
                           "WalEntry: CRC mismatch");
    }

    // Parse the CRC-validated body.
    const uint8_t* bp = data + body_begin;
    WalEntry entry;
    const uint8_t type_raw = bp[0];
    if (type_raw != static_cast<uint8_t>(Type::kInsert) &&
        type_raw != static_cast<uint8_t>(Type::kDelete)) {
        return PhnswStatus(StatusCode::kInvalidArgument, phnsw_errors::kWalCorrupted,
                           "WalEntry: unknown entry_type");
    }
    entry.type = static_cast<Type>(type_raw);
    entry.timestamp_ms = static_cast<int64_t>(GetU64(bp + kTypeBytes));
    entry.block_id = GetU64(bp + kTypeBytes + kTimestampBytes);

    if (entry.type == Type::kInsert) {
        const size_t vec_bytes = body_len - kFixedBodyBytes;
        if (vec_bytes % sizeof(float) != 0) {
            return PhnswStatus(StatusCode::kInvalidArgument, phnsw_errors::kWalCorrupted,
                               "WalEntry: INSERT vector not float-aligned");
        }
        const size_t vec_dim = vec_bytes / sizeof(float);
        if (expected_dim > 0 && vec_dim != static_cast<size_t>(expected_dim)) {
            return PhnswStatus(StatusCode::kInvalidArgument,
                               phnsw_errors::kVectorDimMismatch,
                               "WalEntry: INSERT dim mismatch");
        }
        entry.vector.resize(vec_dim);
        const uint8_t* vp = bp + kFixedBodyBytes;
        for (size_t i = 0; i < vec_dim; ++i) {
            const uint32_t bits = GetU32(vp + i * sizeof(float));
            std::memcpy(&entry.vector[i], &bits, sizeof(float));
        }
    } else {
        // DELETE must carry no vector payload.
        if (body_len != kFixedBodyBytes) {
            return PhnswStatus(StatusCode::kInvalidArgument, phnsw_errors::kWalCorrupted,
                               "WalEntry: DELETE carries unexpected payload");
        }
    }

    *offset = frame_end;
    return entry;
}

}  // namespace cortrix::store
