// Index S2 — WalEntry codec tests (design § 6 S2 test matrix).
//
// Covers INSERT/DELETE serialize<->deserialize round-trips, CRC validity and
// tamper detection, exact frame sizes (4121 B INSERT @dim=1024, 25 B DELETE),
// dim-mismatch handling, and the back-to-back DeserializeFrom scan used by S4
// recovery.

#include "cortrix/store/phnsw/wal_entry.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "cortrix/store/group_commit_writer.h"  // Crc32 (shared)

namespace cortrix::store {
namespace {

std::vector<float> MakeVec(int dim, int seed) {
    std::vector<float> v(static_cast<size_t>(dim));
    for (int i = 0; i < dim; ++i) {
        v[static_cast<size_t>(i)] = static_cast<float>((seed * 7 + i) % 251) * 0.013f;
    }
    return v;
}

// Forge a complete frame from an arbitrary CRC-covered body (type..payload),
// computing a *valid* CRC so the parser passes the CRC gate and reaches the
// body-validation branches (unknown type / misaligned vector / DELETE payload).
std::vector<uint8_t> FrameFrom(const std::vector<uint8_t>& body) {
    std::vector<uint8_t> out;
    const uint32_t entry_size = static_cast<uint32_t>(body.size() + 4);  // body + 4B CRC
    auto put_u32 = [&out](uint32_t v) {
        out.push_back(static_cast<uint8_t>(v & 0xFFu));
        out.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
        out.push_back(static_cast<uint8_t>((v >> 16) & 0xFFu));
        out.push_back(static_cast<uint8_t>((v >> 24) & 0xFFu));
    };
    put_u32(entry_size);
    out.insert(out.end(), body.begin(), body.end());
    put_u32(Crc32(body.data(), body.size()));
    return out;
}

// A fixed 17B body (type + i64 ts + u64 id), optionally extended with `extra`.
std::vector<uint8_t> MakeBody(uint8_t type, const std::vector<uint8_t>& extra = {}) {
    std::vector<uint8_t> body;
    body.push_back(type);
    for (int i = 0; i < 8; ++i) body.push_back(0);   // timestamp i64 = 0
    for (int i = 0; i < 8; ++i) body.push_back(0);   // block_id u64 = 0
    body.insert(body.end(), extra.begin(), extra.end());
    return body;
}

TEST(WalEntryTest, InsertEntry_Serialize_Deserialize) {
    auto vec = MakeVec(1024, 1);
    auto bytes = WalEntry::SerializeInsert(/*block_id=*/42, vec.data(), 1024, /*ts=*/12345);
    auto r = WalEntry::Deserialize(bytes, 1024);
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_EQ(r.value().type, WalEntry::Type::kInsert);
    EXPECT_EQ(r.value().block_id, 42u);
    EXPECT_EQ(r.value().timestamp_ms, 12345);
    ASSERT_EQ(r.value().vector.size(), 1024u);
    for (int i = 0; i < 1024; ++i) {
        EXPECT_FLOAT_EQ(r.value().vector[static_cast<size_t>(i)], vec[static_cast<size_t>(i)]);
    }
}

TEST(WalEntryTest, DeleteEntry_Serialize_Deserialize) {
    auto bytes = WalEntry::SerializeDelete(/*block_id=*/777, /*ts=*/9999);
    auto r = WalEntry::Deserialize(bytes);
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_EQ(r.value().type, WalEntry::Type::kDelete);
    EXPECT_EQ(r.value().block_id, 777u);
    EXPECT_EQ(r.value().timestamp_ms, 9999);
    EXPECT_TRUE(r.value().vector.empty());
}

TEST(WalEntryTest, InsertEntry_CRC_Valid) {
    auto vec = MakeVec(16, 3);
    auto bytes = WalEntry::SerializeInsert(1, vec.data(), 16);
    EXPECT_TRUE(WalEntry::Deserialize(bytes, 16).ok());
}

TEST(WalEntryTest, InsertEntry_CRC_Tampered) {
    auto vec = MakeVec(16, 3);
    auto bytes = WalEntry::SerializeInsert(1, vec.data(), 16);
    // Flip a byte in the vector payload region (past the 4B size + 17B fixed body).
    bytes[10] ^= 0xFF;
    auto r = WalEntry::Deserialize(bytes, 16);
    EXPECT_FALSE(r.ok());
    EXPECT_NE(r.status().message().find("CX_ERR_PHNSW_WAL_CORRUPTED"), std::string::npos);
}

TEST(WalEntryTest, DeleteEntry_CRC_Tampered) {
    auto bytes = WalEntry::SerializeDelete(5);
    bytes[6] ^= 0xFF;  // corrupt the block_id region
    EXPECT_FALSE(WalEntry::Deserialize(bytes).ok());
}

TEST(WalEntryTest, InsertEntry_DimMismatch) {
    auto vec = MakeVec(512, 2);
    auto bytes = WalEntry::SerializeInsert(1, vec.data(), 512);
    // Frame holds 512 floats; ask for 1024 → dim mismatch (CRC still valid).
    auto r = WalEntry::Deserialize(bytes, 1024);
    EXPECT_FALSE(r.ok());
    EXPECT_NE(r.status().message().find("CX_ERR_PHNSW_VECTOR_DIM_MISMATCH"), std::string::npos);
}

TEST(WalEntryTest, DeleteEntry_MinimalSize) {
    auto bytes = WalEntry::SerializeDelete(1);
    // 4 (size) + 1 (type) + 8 (ts) + 8 (id) + 4 (crc) = 25.
    EXPECT_EQ(bytes.size(), 25u);
}

TEST(WalEntryTest, InsertEntry_ExactSize) {
    auto vec = MakeVec(1024, 1);
    auto bytes = WalEntry::SerializeInsert(1, vec.data(), 1024);
    // 4 + 1 + 8 + 8 + 1024*4 + 4 = 4121.
    EXPECT_EQ(bytes.size(), 4121u);
}

TEST(WalEntryTest, DeserializeFrom_BackToBack) {
    // Two records concatenated; the scan advances offset across both.
    auto v0 = MakeVec(8, 1);
    auto a = WalEntry::SerializeInsert(10, v0.data(), 8, /*ts=*/100);
    auto b = WalEntry::SerializeDelete(20, /*ts=*/200);
    std::vector<uint8_t> buf;
    buf.insert(buf.end(), a.begin(), a.end());
    buf.insert(buf.end(), b.begin(), b.end());

    size_t off = 0;
    auto r0 = WalEntry::DeserializeFrom(buf.data(), buf.size(), &off, 8);
    ASSERT_TRUE(r0.ok());
    EXPECT_EQ(r0.value().block_id, 10u);
    EXPECT_EQ(off, a.size());

    auto r1 = WalEntry::DeserializeFrom(buf.data(), buf.size(), &off, 8);
    ASSERT_TRUE(r1.ok());
    EXPECT_EQ(r1.value().type, WalEntry::Type::kDelete);
    EXPECT_EQ(r1.value().block_id, 20u);
    EXPECT_EQ(off, buf.size());
}

TEST(WalEntryTest, DeserializeFrom_ShortRecordLeavesOffset) {
    auto v = MakeVec(8, 1);
    auto a = WalEntry::SerializeInsert(10, v.data(), 8);
    // Chop the last few bytes → frame extends past buffer.
    std::vector<uint8_t> truncated(a.begin(), a.end() - 5);
    size_t off = 0;
    auto r = WalEntry::DeserializeFrom(truncated.data(), truncated.size(), &off, 8);
    EXPECT_FALSE(r.ok());
    EXPECT_EQ(off, 0u);  // offset unchanged → caller truncates the tail
}

TEST(WalEntryTest, Deserialize_RejectsTrailingBytes) {
    auto bytes = WalEntry::SerializeDelete(1);
    bytes.push_back(0x00);  // extra trailing byte
    EXPECT_FALSE(WalEntry::Deserialize(bytes).ok());
}

TEST(WalEntryTest, Insert_AcceptsDeclaredDimWhenExpectedZero) {
    auto vec = MakeVec(32, 9);
    auto bytes = WalEntry::SerializeInsert(3, vec.data(), 32);
    auto r = WalEntry::Deserialize(bytes, /*expected_dim=*/0);  // accept declared
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value().vector.size(), 32u);
}

// --- branch-coverage supplements (DeserializeFrom body-validation arms) ----

TEST(WalEntryTest, DeserializeFrom_TruncatedSizePrefix) {
    // Fewer than 4 bytes available → "truncated entry_size" branch (distinct from
    // the frame-past-buffer branch, which has a valid size prefix).
    std::vector<uint8_t> buf = {0x01, 0x02};  // 2 bytes < kEntrySizeBytes(4)
    size_t off = 0;
    auto r = WalEntry::DeserializeFrom(buf.data(), buf.size(), &off, 8);
    EXPECT_FALSE(r.ok());
    EXPECT_NE(r.status().message().find("truncated entry_size"), std::string::npos);
    EXPECT_EQ(off, 0u);
}

TEST(WalEntryTest, DeserializeFrom_EntrySizeBelowMinimum) {
    // A valid 4B prefix declaring an entry_size smaller than the fixed body+CRC
    // → "entry_size below minimum" branch.
    std::vector<uint8_t> buf = {0x05, 0x00, 0x00, 0x00,  // entry_size = 5 (< 17+4)
                                0xAA, 0xBB, 0xCC, 0xDD, 0xEE};
    size_t off = 0;
    auto r = WalEntry::DeserializeFrom(buf.data(), buf.size(), &off, 8);
    EXPECT_FALSE(r.ok());
    EXPECT_NE(r.status().message().find("entry_size below minimum"), std::string::npos);
}

TEST(WalEntryTest, DeserializeFrom_UnknownType_WithValidCrc) {
    // CRC valid but type byte is neither kInsert nor kDelete → "unknown entry_type".
    auto frame = FrameFrom(MakeBody(/*type=*/0x7F));  // bogus type, no payload
    size_t off = 0;
    auto r = WalEntry::DeserializeFrom(frame.data(), frame.size(), &off, 0);
    EXPECT_FALSE(r.ok());
    EXPECT_NE(r.status().message().find("unknown entry_type"), std::string::npos);
}

TEST(WalEntryTest, DeserializeFrom_InsertVectorNotFloatAligned) {
    // INSERT body whose vector region is not a multiple of sizeof(float) (3 extra
    // bytes) → "INSERT vector not float-aligned" (CRC valid so it reaches here).
    const uint8_t kInsert = static_cast<uint8_t>(WalEntry::Type::kInsert);
    auto frame = FrameFrom(MakeBody(kInsert, /*extra=*/{0x01, 0x02, 0x03}));
    size_t off = 0;
    auto r = WalEntry::DeserializeFrom(frame.data(), frame.size(), &off, 0);
    EXPECT_FALSE(r.ok());
    EXPECT_NE(r.status().message().find("not float-aligned"), std::string::npos);
}

TEST(WalEntryTest, DeserializeFrom_DeleteWithPayloadRejected) {
    // DELETE must carry no vector payload; 4 extra (float-aligned) bytes still
    // trip the "DELETE carries unexpected payload" branch.
    const uint8_t kDelete = static_cast<uint8_t>(WalEntry::Type::kDelete);
    auto frame = FrameFrom(MakeBody(kDelete, /*extra=*/{0x00, 0x00, 0x80, 0x3F}));
    size_t off = 0;
    auto r = WalEntry::DeserializeFrom(frame.data(), frame.size(), &off, 0);
    EXPECT_FALSE(r.ok());
    EXPECT_NE(r.status().message().find("DELETE carries unexpected payload"), std::string::npos);
}

TEST(WalEntryTest, DeserializeFrom_InsertZeroLengthVectorAllowed) {
    // A 0-dim INSERT (body_len == kFixedBodyBytes) is float-aligned (vec_bytes==0)
    // and parses to an empty vector — the "aligned, vec_dim==0" success path.
    const uint8_t kInsert = static_cast<uint8_t>(WalEntry::Type::kInsert);
    auto frame = FrameFrom(MakeBody(kInsert));  // no payload
    size_t off = 0;
    auto r = WalEntry::DeserializeFrom(frame.data(), frame.size(), &off, 0);
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_EQ(r.value().type, WalEntry::Type::kInsert);
    EXPECT_TRUE(r.value().vector.empty());
}

TEST(WalEntryTest, Serialize_AutoTimestampWhenNegative) {
    // timestamp_ms < 0 selects the wall-clock branch in NowMsOrGiven; the recorded
    // timestamp is then a real (positive) epoch-ms value, not the sentinel.
    auto vec = MakeVec(8, 1);
    auto bytes = WalEntry::SerializeInsert(1, vec.data(), 8, /*timestamp_ms=*/-1);
    auto r = WalEntry::Deserialize(bytes, 8);
    ASSERT_TRUE(r.ok());
    EXPECT_GT(r.value().timestamp_ms, 0);
    // An explicit non-negative timestamp is preserved verbatim (the other arm).
    auto bytes2 = WalEntry::SerializeDelete(2, /*timestamp_ms=*/0);
    auto r2 = WalEntry::Deserialize(bytes2);
    ASSERT_TRUE(r2.ok());
    EXPECT_EQ(r2.value().timestamp_ms, 0);
}

}  // namespace
}  // namespace cortrix::store
