#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <vector>

#include "cortrix/common/status.h"
#include "cortrix/retrieval/sparse_codec.h"

// Value-matrix breadth coverage for the F40 sparse_vec on-disk codec
// (SerializeSparseVec / DeserializeSparseVec / SerializedSparseVecSize).
//
// Real wire format (sparse_codec.h / sparse_codec.cpp, verified):
//   [uint16 num_terms] then num_terms x ([uint16 term_id][float32 weight]), LE.
//   size = 2 + terms * 6.
//   - term_id key type is uint32_t in SparseVector::terms; Serialize REJECTS any
//     key > 0xFFFF (bool* ok = false, empty buffer returned). std::map drops
//     duplicate keys at construction, so on-disk ids are unique by definition.
//   - Deserialize: empty/nullptr buffer -> empty vector (NOT an error).
//     len < 2 -> reject. len < declared(2+n*6) -> reject (truncation).
//     OVERSIZE (extra trailing bytes) is IGNORED, not rejected (len < expected
//     is the only guard) -> asserted as documented behavior below.
//   - weights are memcpy'd both ways: NaN/Inf/negative are bit-preserved.
//
// All suite + fixture names are globally unique (SparseCodecValMatrix* prefix).

namespace cortrix::retrieval {
namespace {

// ---- helpers -------------------------------------------------------------

SparseVector VecOf(std::map<uint32_t, float> terms) {
    SparseVector v;
    v.terms = std::move(terms);
    return v;
}

// Bit-equal float compare (so NaN==NaN by payload, +0 != -0 if bits differ).
bool BitEq(float a, float b) {
    uint32_t ba, bb;
    std::memcpy(&ba, &a, 4);
    std::memcpy(&bb, &b, 4);
    return ba == bb;
}

// ==========================================================================
// Size accounting
// ==========================================================================
struct SparseCodecValMatrixSizeCase {
    size_t num_terms;
    size_t expected_bytes;
};

class SparseCodecValMatrixSize
    : public ::testing::TestWithParam<SparseCodecValMatrixSizeCase> {};

TEST_P(SparseCodecValMatrixSize, SizeFormulaAndActual) {
    const auto& tc = GetParam();
    std::map<uint32_t, float> terms;
    for (uint32_t i = 0; i < tc.num_terms; ++i) {
        terms[i] = 0.1f * static_cast<float>(i + 1);
    }
    SparseVector v = VecOf(terms);
    EXPECT_EQ(SerializedSparseVecSize(v), tc.expected_bytes);
    bool ok = false;
    std::vector<uint8_t> blob = SerializeSparseVec(v, &ok);
    EXPECT_TRUE(ok);
    EXPECT_EQ(blob.size(), tc.expected_bytes);
}

INSTANTIATE_TEST_SUITE_P(
    Matrix, SparseCodecValMatrixSize,
    ::testing::Values(
        SparseCodecValMatrixSizeCase{0, 2},
        SparseCodecValMatrixSizeCase{1, 8},
        SparseCodecValMatrixSizeCase{2, 14},
        SparseCodecValMatrixSizeCase{3, 20},
        SparseCodecValMatrixSizeCase{10, 62},
        SparseCodecValMatrixSizeCase{100, 602},
        SparseCodecValMatrixSizeCase{200, 1202}));

// ==========================================================================
// term_id boundary round-trip (0, 1, mid, 0xFFFE, 0xFFFF all in-range)
// ==========================================================================
class SparseCodecValMatrixTermIdRoundTrip
    : public ::testing::TestWithParam<uint32_t> {};

TEST_P(SparseCodecValMatrixTermIdRoundTrip, SingleTermRoundTrips) {
    const uint32_t id = GetParam();
    SparseVector v = VecOf({{id, 0.625f}});
    bool ok = false;
    std::vector<uint8_t> blob = SerializeSparseVec(v, &ok);
    ASSERT_TRUE(ok);
    ASSERT_EQ(blob.size(), 8u);
    Result<SparseVector> got = DeserializeSparseVec(blob);
    ASSERT_TRUE(got.ok());
    const SparseVector& out = got.value();
    ASSERT_EQ(out.size(), 1u);
    ASSERT_EQ(out.terms.count(id), 1u);
    EXPECT_TRUE(BitEq(out.terms.at(id), 0.625f));
}

INSTANTIATE_TEST_SUITE_P(Matrix, SparseCodecValMatrixTermIdRoundTrip,
                         ::testing::Values<uint32_t>(0u, 1u, 2u, 255u, 256u,
                                                     1000u, 32768u, 0xFFFEu,
                                                     0xFFFFu));

// ==========================================================================
// term_id out-of-range -> Serialize REJECTS (ok=false, empty buffer)
// ==========================================================================
class SparseCodecValMatrixTermIdReject
    : public ::testing::TestWithParam<uint32_t> {};

TEST_P(SparseCodecValMatrixTermIdReject, OutOfRangeIdRejected) {
    const uint32_t id = GetParam();
    SparseVector v = VecOf({{id, 1.0f}});
    bool ok = true;
    std::vector<uint8_t> blob = SerializeSparseVec(v, &ok);
    EXPECT_FALSE(ok);
    EXPECT_TRUE(blob.empty());
}

INSTANTIATE_TEST_SUITE_P(Matrix, SparseCodecValMatrixTermIdReject,
                         ::testing::Values<uint32_t>(0x10000u, 0x10001u,
                                                     0x20000u, 250000u,
                                                     0xFFFFFFFFu));

TEST(SparseCodecValMatrixTermIdReject, RejectWithNullOkPtrDoesNotCrash) {
    SparseVector v = VecOf({{0x10000u, 1.0f}});
    std::vector<uint8_t> blob = SerializeSparseVec(v, nullptr);
    EXPECT_TRUE(blob.empty());
}

TEST(SparseCodecValMatrixTermIdReject, OneBadIdAmongGoodRejectsWhole) {
    SparseVector v = VecOf({{1u, 0.1f}, {2u, 0.2f}, {0x10000u, 0.3f}});
    bool ok = true;
    std::vector<uint8_t> blob = SerializeSparseVec(v, &ok);
    EXPECT_FALSE(ok);
    EXPECT_TRUE(blob.empty());
}

// ==========================================================================
// weight value matrix: 0, -0, tiny, large, denormal, negative, NaN, Inf
// bit-preserving round-trip (memcpy both directions).
// ==========================================================================
class SparseCodecValMatrixWeight : public ::testing::TestWithParam<float> {};

TEST_P(SparseCodecValMatrixWeight, WeightBitPreserved) {
    const float w = GetParam();
    SparseVector v = VecOf({{42u, w}});
    bool ok = false;
    std::vector<uint8_t> blob = SerializeSparseVec(v, &ok);
    ASSERT_TRUE(ok);
    Result<SparseVector> got = DeserializeSparseVec(blob);
    ASSERT_TRUE(got.ok());
    const SparseVector& out = got.value();
    ASSERT_EQ(out.size(), 1u);
    EXPECT_TRUE(BitEq(out.terms.at(42u), w))
        << "weight not bit-preserved for input value";
}

INSTANTIATE_TEST_SUITE_P(
    Matrix, SparseCodecValMatrixWeight,
    ::testing::Values(
        0.0f,
        -0.0f,
        1.0f,
        -1.0f,
        0.5f,
        std::numeric_limits<float>::min(),         // smallest normal
        std::numeric_limits<float>::denorm_min(),  // smallest subnormal
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::epsilon(),
        1e-30f, 1e30f, -1e30f,
        std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity()));

// NaN handled separately to assert quiet-NaN payload preservation explicitly.
TEST(SparseCodecValMatrixWeightNaN, QuietNaNBitPreserved) {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    SparseVector v = VecOf({{7u, nan}});
    bool ok = false;
    std::vector<uint8_t> blob = SerializeSparseVec(v, &ok);
    ASSERT_TRUE(ok);
    Result<SparseVector> got = DeserializeSparseVec(blob);
    ASSERT_TRUE(got.ok());
    const float out = got.value().terms.at(7u);
    EXPECT_TRUE(std::isnan(out));
    EXPECT_TRUE(BitEq(out, nan));
}

TEST(SparseCodecValMatrixWeightNaN, MixedSpecialWeightsRoundTrip) {
    SparseVector v = VecOf({
        {1u, std::numeric_limits<float>::infinity()},
        {2u, -std::numeric_limits<float>::infinity()},
        {3u, 0.0f},
        {4u, -123.456f},
        {5u, std::numeric_limits<float>::max()},
    });
    bool ok = false;
    std::vector<uint8_t> blob = SerializeSparseVec(v, &ok);
    ASSERT_TRUE(ok);
    Result<SparseVector> got = DeserializeSparseVec(blob);
    ASSERT_TRUE(got.ok());
    const SparseVector& out = got.value();
    ASSERT_EQ(out.size(), 5u);
    EXPECT_TRUE(BitEq(out.terms.at(1u), std::numeric_limits<float>::infinity()));
    EXPECT_TRUE(BitEq(out.terms.at(2u), -std::numeric_limits<float>::infinity()));
    EXPECT_TRUE(BitEq(out.terms.at(3u), 0.0f));
    EXPECT_TRUE(BitEq(out.terms.at(4u), -123.456f));
    EXPECT_TRUE(BitEq(out.terms.at(5u), std::numeric_limits<float>::max()));
}

// ==========================================================================
// num_terms boundary round-trip (0, 1, 2, 100, 200, 1000)
// ==========================================================================
class SparseCodecValMatrixNumTerms : public ::testing::TestWithParam<size_t> {};

TEST_P(SparseCodecValMatrixNumTerms, FullRoundTrip) {
    const size_t n = GetParam();
    std::map<uint32_t, float> terms;
    for (uint32_t i = 0; i < n; ++i) {
        terms[i] = static_cast<float>(i) * 0.01f + 0.001f;
    }
    SparseVector v = VecOf(terms);
    bool ok = false;
    std::vector<uint8_t> blob = SerializeSparseVec(v, &ok);
    ASSERT_TRUE(ok);
    EXPECT_EQ(blob.size(), 2u + n * 6u);
    Result<SparseVector> got = DeserializeSparseVec(blob);
    ASSERT_TRUE(got.ok());
    const SparseVector& out = got.value();
    ASSERT_EQ(out.size(), n);
    for (uint32_t i = 0; i < n; ++i) {
        EXPECT_TRUE(BitEq(out.terms.at(i), static_cast<float>(i) * 0.01f + 0.001f));
    }
}

INSTANTIATE_TEST_SUITE_P(Matrix, SparseCodecValMatrixNumTerms,
                         ::testing::Values<size_t>(0, 1, 2, 3, 16, 100, 200,
                                                   1000));

// Empty vector serializes to the 2-byte header and round-trips empty.
TEST(SparseCodecValMatrixEmpty, EmptyVectorHeaderOnly) {
    SparseVector v = VecOf({});
    bool ok = false;
    std::vector<uint8_t> blob = SerializeSparseVec(v, &ok);
    ASSERT_TRUE(ok);
    ASSERT_EQ(blob.size(), 2u);
    EXPECT_EQ(blob[0], 0u);
    EXPECT_EQ(blob[1], 0u);
    Result<SparseVector> got = DeserializeSparseVec(blob);
    ASSERT_TRUE(got.ok());
    EXPECT_TRUE(got.value().empty());
}

// ==========================================================================
// Duplicate-id semantics: std::map dedups at construction (last value wins).
// On-disk count == unique key count.
// ==========================================================================
TEST(SparseCodecValMatrixDuplicate, MapDedupsLastValueWins) {
    std::map<uint32_t, float> terms;
    terms[5u] = 1.0f;
    terms[5u] = 2.0f;  // overwrites
    terms[5u] = 3.0f;  // last wins
    SparseVector v = VecOf(terms);
    ASSERT_EQ(v.size(), 1u);
    bool ok = false;
    std::vector<uint8_t> blob = SerializeSparseVec(v, &ok);
    ASSERT_TRUE(ok);
    ASSERT_EQ(blob.size(), 8u);  // 1 term
    Result<SparseVector> got = DeserializeSparseVec(blob);
    ASSERT_TRUE(got.ok());
    ASSERT_EQ(got.value().size(), 1u);
    EXPECT_TRUE(BitEq(got.value().terms.at(5u), 3.0f));
}

// ==========================================================================
// Empty / NULL-column inputs deserialize to empty vector (not an error).
// ==========================================================================
TEST(SparseCodecValMatrixDeserEmpty, ZeroLengthBufferIsEmptyOk) {
    std::vector<uint8_t> empty;
    Result<SparseVector> got = DeserializeSparseVec(empty);
    ASSERT_TRUE(got.ok());
    EXPECT_TRUE(got.value().empty());
}

TEST(SparseCodecValMatrixDeserEmpty, NullPointerIsEmptyOk) {
    Result<SparseVector> got = DeserializeSparseVec(nullptr, 0);
    ASSERT_TRUE(got.ok());
    EXPECT_TRUE(got.value().empty());
}

TEST(SparseCodecValMatrixDeserEmpty, NullPointerNonzeroLenIsEmptyOk) {
    // Impl short-circuits on data==nullptr || len==0 before reading.
    Result<SparseVector> got = DeserializeSparseVec(nullptr, 100);
    ASSERT_TRUE(got.ok());
    EXPECT_TRUE(got.value().empty());
}

// ==========================================================================
// Truncated blob matrix -> REJECT (not crash). status = kInvalidArgument
// (SparseStatus maps kSparseSerializeFailed -> InvalidArgument; verified via
// SparseErrorToStatusCode chain — assert not-ok + message non-empty).
// ==========================================================================
struct SparseCodecValMatrixTruncCase {
    uint16_t declared_num_terms;
    size_t actual_len;  // total bytes provided (must be < 2 + n*6 to reject)
};

class SparseCodecValMatrixTruncated
    : public ::testing::TestWithParam<SparseCodecValMatrixTruncCase> {};

TEST_P(SparseCodecValMatrixTruncated, TruncatedRejectsNotCrash) {
    const auto& tc = GetParam();
    std::vector<uint8_t> blob(tc.actual_len, 0u);
    if (tc.actual_len >= 2) {
        blob[0] = static_cast<uint8_t>(tc.declared_num_terms & 0xFF);
        blob[1] = static_cast<uint8_t>((tc.declared_num_terms >> 8) & 0xFF);
    }
    Result<SparseVector> got = DeserializeSparseVec(blob);
    EXPECT_FALSE(got.ok());
    EXPECT_FALSE(got.status().message().empty());
}

INSTANTIATE_TEST_SUITE_P(
    Matrix, SparseCodecValMatrixTruncated,
    ::testing::Values(
        SparseCodecValMatrixTruncCase{0, 1},     // 1 byte < 2-byte header
        SparseCodecValMatrixTruncCase{1, 2},     // header says 1 term, 0 body
        SparseCodecValMatrixTruncCase{1, 7},     // need 8, have 7
        SparseCodecValMatrixTruncCase{2, 8},     // need 14, have 8
        SparseCodecValMatrixTruncCase{10, 20},   // need 62, have 20
        SparseCodecValMatrixTruncCase{100, 100},  // need 602, have 100
        SparseCodecValMatrixTruncCase{0xFFFF, 600}));

// One-byte buffer (below header) rejects.
TEST(SparseCodecValMatrixTruncated, SingleByteBufferRejects) {
    std::vector<uint8_t> blob{0x05};
    Result<SparseVector> got = DeserializeSparseVec(blob);
    EXPECT_FALSE(got.ok());
}

// Exactly-enough buffer succeeds (boundary: len == expected).
TEST(SparseCodecValMatrixTruncated, ExactLengthSucceeds) {
    SparseVector v = VecOf({{1u, 0.5f}, {2u, 0.25f}});
    std::vector<uint8_t> blob = SerializeSparseVec(v);
    ASSERT_EQ(blob.size(), 14u);
    Result<SparseVector> got = DeserializeSparseVec(blob.data(), 14u);
    EXPECT_TRUE(got.ok());
    EXPECT_EQ(got.value().size(), 2u);
}

// ==========================================================================
// Oversize blob matrix: extra trailing bytes are REJECTED. The serializer
// always writes exactly `expected` bytes, so a longer blob read back from
// storage is corruption just like a shorter one (#32 audit item A5; the old
// pin asserted the tolerated junk as required behavior).
// ==========================================================================
class SparseCodecValMatrixOversize : public ::testing::TestWithParam<size_t> {};

TEST_P(SparseCodecValMatrixOversize, ExtraTrailingBytesRejected) {
    const size_t extra = GetParam();
    SparseVector v = VecOf({{1u, 1.5f}, {2u, 2.5f}, {3u, 3.5f}});
    std::vector<uint8_t> blob = SerializeSparseVec(v);
    ASSERT_EQ(blob.size(), 20u);  // 2 + 3*6
    blob.resize(blob.size() + extra, 0xAB);  // append junk
    Result<SparseVector> got = DeserializeSparseVec(blob);
    ASSERT_FALSE(got.ok()) << "oversize blob is corruption and must be rejected";
    EXPECT_NE(got.status().message().find("oversize"), std::string::npos);
}

INSTANTIATE_TEST_SUITE_P(Matrix, SparseCodecValMatrixOversize,
                         ::testing::Values<size_t>(1, 2, 6, 7, 100, 1000));

// ==========================================================================
// Pointer/length overload parity with the vector overload.
// ==========================================================================
TEST(SparseCodecValMatrixOverload, PtrLenMatchesVector) {
    SparseVector v = VecOf({{10u, 0.1f}, {20u, 0.2f}, {30u, 0.3f}});
    std::vector<uint8_t> blob = SerializeSparseVec(v);
    Result<SparseVector> a = DeserializeSparseVec(blob);
    Result<SparseVector> b = DeserializeSparseVec(blob.data(), blob.size());
    ASSERT_TRUE(a.ok());
    ASSERT_TRUE(b.ok());
    EXPECT_EQ(a.value().size(), b.value().size());
    for (const auto& [id, w] : a.value().terms) {
        ASSERT_EQ(b.value().terms.count(id), 1u);
        EXPECT_TRUE(BitEq(b.value().terms.at(id), w));
    }
}

// ==========================================================================
// Little-endian byte layout assertion: known id/weight -> known bytes.
// ==========================================================================
TEST(SparseCodecValMatrixWire, LittleEndianLayout) {
    SparseVector v = VecOf({{0x0102u, 1.0f}});  // id bytes LE: 02 01
    std::vector<uint8_t> blob = SerializeSparseVec(v);
    ASSERT_EQ(blob.size(), 8u);
    EXPECT_EQ(blob[0], 0x01u);  // num_terms low
    EXPECT_EQ(blob[1], 0x00u);  // num_terms high
    EXPECT_EQ(blob[2], 0x02u);  // term_id low
    EXPECT_EQ(blob[3], 0x01u);  // term_id high
    // 1.0f = 0x3F800000 LE -> 00 00 80 3F
    EXPECT_EQ(blob[4], 0x00u);
    EXPECT_EQ(blob[5], 0x00u);
    EXPECT_EQ(blob[6], 0x80u);
    EXPECT_EQ(blob[7], 0x3Fu);
}

}  // namespace
}  // namespace cortrix::retrieval
