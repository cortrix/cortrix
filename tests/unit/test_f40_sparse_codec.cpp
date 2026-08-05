#include <gtest/gtest.h>

#include "cortrix/common/block_types.h"
#include "cortrix/retrieval/sparse_codec.h"

// Sparse retrieval S3 — sparse_vec BLOB serialize/deserialize round-trip + block header flags_ext
// has_sparse_vec semantics. Wire format (§4.2): [u16 num_terms]([u16 id][f32 w])*.
namespace cortrix::retrieval {
namespace {

SparseVector MakeVec(std::map<uint32_t, float> terms) {
    SparseVector v;
    v.terms = std::move(terms);
    return v;
}

// ---------- size accounting ----------

TEST(F40SparseCodecTest, SerializedSize) {
    EXPECT_EQ(SerializedSparseVecSize(MakeVec({})), 2u);            // header only
    EXPECT_EQ(SerializedSparseVecSize(MakeVec({{1, 0.5f}})), 8u);   // 2 + 6
    EXPECT_EQ(SerializedSparseVecSize(MakeVec({{1, 0.5f}, {2, 0.25f}})), 14u);
}

TEST(F40SparseCodecTest, K100SizeMatchesDesign) {
    // §4.2: K=100 → 2 + 100×6 = 602 bytes.
    std::map<uint32_t, float> terms;
    for (uint32_t i = 1; i <= 100; ++i) terms[i] = 0.1f * static_cast<float>(i);
    EXPECT_EQ(SerializedSparseVecSize(MakeVec(terms)), 602u);
    EXPECT_EQ(SerializeSparseVec(MakeVec(terms)).size(), 602u);
}

// ---------- round-trip ----------

TEST(F40SparseCodecTest, RoundTripBasic) {
    SparseVector in = MakeVec({{12345, 0.85f}, {6789, 0.62f}, {42, 0.1f}});
    bool ok = false;
    auto blob = SerializeSparseVec(in, &ok);
    ASSERT_TRUE(ok);

    auto out = DeserializeSparseVec(blob);
    ASSERT_TRUE(out.ok()) << out.status().message();
    EXPECT_EQ(out.value().terms, in.terms);
}

TEST(F40SparseCodecTest, RoundTripPreservesFloatBits) {
    SparseVector in = MakeVec({{1, 0.123456789f}, {2, 0.987654321f}});
    auto out = DeserializeSparseVec(SerializeSparseVec(in));
    ASSERT_TRUE(out.ok());
    // float32 round-trips exactly (same storage type both ways).
    EXPECT_FLOAT_EQ(out.value().terms.at(1), 0.123456789f);
    EXPECT_FLOAT_EQ(out.value().terms.at(2), 0.987654321f);
}

TEST(F40SparseCodecTest, RoundTripBoundaryTermIds) {
    SparseVector in = MakeVec({{1, 0.5f}, {65535, 0.9f}});  // min active + max u16
    bool ok = false;
    auto blob = SerializeSparseVec(in, &ok);
    ASSERT_TRUE(ok);
    auto out = DeserializeSparseVec(blob);
    ASSERT_TRUE(out.ok());
    EXPECT_EQ(out.value().terms, in.terms);
}

TEST(F40SparseCodecTest, EmptyVectorRoundTrip) {
    bool ok = false;
    auto blob = SerializeSparseVec(MakeVec({}), &ok);
    ASSERT_TRUE(ok);
    EXPECT_EQ(blob.size(), 2u);  // num_terms=0
    auto out = DeserializeSparseVec(blob);
    ASSERT_TRUE(out.ok());
    EXPECT_TRUE(out.value().empty());
}

TEST(F40SparseCodecTest, RawPointerOverload) {
    SparseVector in = MakeVec({{7, 0.3f}, {8, 0.4f}});
    auto blob = SerializeSparseVec(in);
    auto out = DeserializeSparseVec(blob.data(), blob.size());
    ASSERT_TRUE(out.ok());
    EXPECT_EQ(out.value().terms, in.terms);
}

// ---------- NULL / empty input ----------

TEST(F40SparseCodecTest, NullBufferIsEmptyNotError) {
    auto out = DeserializeSparseVec(nullptr, 0);
    ASSERT_TRUE(out.ok());  // a NULL column → empty, not a parse error
    EXPECT_TRUE(out.value().empty());
}

TEST(F40SparseCodecTest, ZeroLenIsEmptyNotError) {
    std::vector<uint8_t> empty;
    auto out = DeserializeSparseVec(empty);
    ASSERT_TRUE(out.ok());
    EXPECT_TRUE(out.value().empty());
}

// ---------- malformed input → SERIALIZE_FAILED ----------

TEST(F40SparseCodecTest, TruncatedHeaderRejected) {
    std::vector<uint8_t> one_byte{0x01};  // < 2 bytes for the num_terms header
    auto out = DeserializeSparseVec(one_byte);
    ASSERT_FALSE(out.ok());
    EXPECT_NE(out.status().message().find("CX_ERR_SPARSE_SERIALIZE_FAILED"),
              std::string::npos);
}

TEST(F40SparseCodecTest, TruncatedBodyRejected) {
    // Header says 5 terms but the body holds only a partial entry.
    std::vector<uint8_t> blob{0x05, 0x00, 0x01, 0x00, 0x00};
    auto out = DeserializeSparseVec(blob);
    ASSERT_FALSE(out.ok());
    EXPECT_NE(out.status().message().find("truncated"), std::string::npos);
}

TEST(F40SparseCodecTest, TermIdOutOfUint16RangeRejected) {
    SparseVector in = MakeVec({{70000u, 0.5f}});  // > 65535 → cannot serialize
    bool ok = true;
    auto blob = SerializeSparseVec(in, &ok);
    EXPECT_FALSE(ok);
    EXPECT_TRUE(blob.empty());
}

// ---------- block header flags_ext has_sparse_vec semantics ----------

TEST(F40SparseCodecTest, FlagBitValueIsBit4) {
    // Sparse retrieval owns flags_ext bit4 = 0x10 (block_types.h:49); pin it so a later edit
    // to the shared enum that moved the bit would fail here.
    EXPECT_EQ(static_cast<uint8_t>(kFlagExtHasSparseVec), 0x10);
}

TEST(F40SparseCodecTest, NonEmptySparseSetsFlagEmptyClears) {
    // The writer (S5/S9) sets has_sparse_vec iff the vector is non-empty; model
    // that single rule against the codec's empty() so both stay consistent.
    uint8_t flags_ext = 0;

    SparseVector nonempty = MakeVec({{1, 0.5f}});
    if (!nonempty.empty()) flags_ext |= kFlagExtHasSparseVec;
    EXPECT_TRUE(flags_ext & kFlagExtHasSparseVec);

    flags_ext = kFlagExtHasSparseVec;  // pretend previously set
    SparseVector empty = MakeVec({});
    if (empty.empty()) flags_ext &= ~static_cast<uint8_t>(kFlagExtHasSparseVec);
    EXPECT_FALSE(flags_ext & kFlagExtHasSparseVec);
}

TEST(F40SparseCodecTest, SparseFlagDoesNotDisturbOtherExtBits) {
    // Setting/clearing has_sparse_vec must leave contextualized (bit3) etc.
    uint8_t flags_ext = kFlagExtHasContextualized | kFlagExtHasEntities;  // 0x08|0x01
    flags_ext |= kFlagExtHasSparseVec;                                    // +0x10
    EXPECT_TRUE(flags_ext & kFlagExtHasContextualized);
    EXPECT_TRUE(flags_ext & kFlagExtHasEntities);
    flags_ext &= ~static_cast<uint8_t>(kFlagExtHasSparseVec);
    EXPECT_TRUE(flags_ext & kFlagExtHasContextualized);  // untouched
    EXPECT_TRUE(flags_ext & kFlagExtHasEntities);
    EXPECT_FALSE(flags_ext & kFlagExtHasSparseVec);
}

}  // namespace
}  // namespace cortrix::retrieval
