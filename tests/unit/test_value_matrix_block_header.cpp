#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "cortrix/common/block_header.h"
#include "cortrix/common/block_types.h"

// Value-matrix breadth coverage for the F09 128-byte block header codec
// (BlockBuild / BlockParse / BlockComputeCrc32 / BlockHeaderToString).
//
// Real behavior (block_header.h / src/store/block_header.cpp, verified):
//   - 128-byte packed struct; magic kBlockMagic=0x43525458 ("CRTX"),
//     header_version=1, header_size=128.
//   - BlockBuild sets flags from payload presence:
//       content>0 -> kFlagHasContent; metadata>0 -> kFlagHasMetadata;
//       chain>0 -> kFlagHasChain; vector_dim>0 -> kFlagHasVector;
//       content>0 && level in {L2,L3} -> kFlagHasFts.
//     content_offset/metadata_offset/chain_offset are RELATIVE to header end.
//   - CRC32 covers header[0..79] + header[84..127] + payload (skips crc32 @80).
//   - BlockParse rejects: data==nullptr, len<128, hdr==nullptr, bad magic,
//     header_version!=1, total_size>len, crc mismatch. Else points hdr in place.
//
// All suite + fixture names globally unique (BlockHeaderValMatrix* prefix).

namespace cortrix {
namespace {

// ==========================================================================
// Build/parse round-trip over (content, metadata, chain) presence matrix.
// ==========================================================================
struct BlockHeaderValMatrixPayloadCase {
    std::string content;
    std::string metadata;
    std::string chain;
};

class BlockHeaderValMatrixPayload
    : public ::testing::TestWithParam<BlockHeaderValMatrixPayloadCase> {};

TEST_P(BlockHeaderValMatrixPayload, BuildParseRoundTrip) {
    const auto& tc = GetParam();
    std::vector<uint8_t> blob = BlockBuild(
        kBlockFile, ProcessingLevel::kLevelL3, tc.content, tc.metadata, tc.chain);

    const cortrix_block_header_t* hdr = nullptr;
    ASSERT_TRUE(BlockParse(blob.data(), blob.size(), &hdr));
    ASSERT_NE(hdr, nullptr);

    EXPECT_EQ(hdr->magic, kBlockMagic);
    EXPECT_EQ(hdr->header_version, kHeaderVersion);
    EXPECT_EQ(hdr->header_size, kHeaderSize);
    EXPECT_EQ(hdr->content_length, static_cast<uint32_t>(tc.content.size()));
    EXPECT_EQ(hdr->metadata_length, static_cast<uint32_t>(tc.metadata.size()));
    EXPECT_EQ(hdr->chain_length, static_cast<uint32_t>(tc.chain.size()));
    EXPECT_EQ(hdr->total_size,
              static_cast<uint32_t>(kHeaderSize + tc.content.size() +
                                    tc.metadata.size() + tc.chain.size()));

    // Flag matrix.
    EXPECT_EQ((hdr->flags & kFlagHasContent) != 0, !tc.content.empty());
    EXPECT_EQ((hdr->flags & kFlagHasMetadata) != 0, !tc.metadata.empty());
    EXPECT_EQ((hdr->flags & kFlagHasChain) != 0, !tc.chain.empty());
    // L3 with content -> FTS flag.
    EXPECT_EQ((hdr->flags & kFlagHasFts) != 0, !tc.content.empty());

    // Payload bytes correct (offset relative to header end = byte 128).
    if (!tc.content.empty()) {
        const uint8_t* p = blob.data() + kHeaderSize + hdr->content_offset;
        EXPECT_EQ(std::memcmp(p, tc.content.data(), tc.content.size()), 0);
    }
    if (!tc.metadata.empty()) {
        const uint8_t* p = blob.data() + kHeaderSize + hdr->metadata_offset;
        EXPECT_EQ(std::memcmp(p, tc.metadata.data(), tc.metadata.size()), 0);
    }
    if (!tc.chain.empty()) {
        const uint8_t* p = blob.data() + kHeaderSize + hdr->chain_offset;
        EXPECT_EQ(std::memcmp(p, tc.chain.data(), tc.chain.size()), 0);
    }
}

INSTANTIATE_TEST_SUITE_P(
    Matrix, BlockHeaderValMatrixPayload,
    ::testing::Values(
        BlockHeaderValMatrixPayloadCase{"", "", ""},
        BlockHeaderValMatrixPayloadCase{"hello", "", ""},
        BlockHeaderValMatrixPayloadCase{"", "{\"k\":1}", ""},
        BlockHeaderValMatrixPayloadCase{"", "", "parse>embed"},
        BlockHeaderValMatrixPayloadCase{"c", "m", ""},
        BlockHeaderValMatrixPayloadCase{"c", "", "ch"},
        BlockHeaderValMatrixPayloadCase{"", "m", "ch"},
        BlockHeaderValMatrixPayloadCase{"content text", "{\"a\":\"b\"}",
                                        "docling>ner>embed"},
        BlockHeaderValMatrixPayloadCase{std::string(4096, 'x'),
                                        std::string(256, 'y'),
                                        std::string(64, 'z')}));

// ==========================================================================
// FTS flag depends on level x content. Matrix over all levels.
// ==========================================================================
struct BlockHeaderValMatrixFtsCase {
    ProcessingLevel level;
    bool has_content;
    bool expect_fts;
};

class BlockHeaderValMatrixFts
    : public ::testing::TestWithParam<BlockHeaderValMatrixFtsCase> {};

TEST_P(BlockHeaderValMatrixFts, FtsFlagByLevel) {
    const auto& tc = GetParam();
    std::string content = tc.has_content ? "txt" : "";
    std::vector<uint8_t> blob =
        BlockBuild(kBlockFile, tc.level, content, "", "");
    const cortrix_block_header_t* hdr = nullptr;
    ASSERT_TRUE(BlockParse(blob.data(), blob.size(), &hdr));
    EXPECT_EQ((hdr->flags & kFlagHasFts) != 0, tc.expect_fts);
}

INSTANTIATE_TEST_SUITE_P(
    Matrix, BlockHeaderValMatrixFts,
    ::testing::Values(
        BlockHeaderValMatrixFtsCase{ProcessingLevel::kLevelL0, true, false},
        BlockHeaderValMatrixFtsCase{ProcessingLevel::kLevelL1, true, false},
        BlockHeaderValMatrixFtsCase{ProcessingLevel::kLevelL2, true, true},
        BlockHeaderValMatrixFtsCase{ProcessingLevel::kLevelL3, true, true},
        BlockHeaderValMatrixFtsCase{ProcessingLevel::kLevelL4, true, false},
        BlockHeaderValMatrixFtsCase{ProcessingLevel::kLevelL2, false, false},
        BlockHeaderValMatrixFtsCase{ProcessingLevel::kLevelL3, false, false}));

// ==========================================================================
// vector_dim -> kFlagHasVector + field round-trip matrix.
// ==========================================================================
class BlockHeaderValMatrixVectorDim
    : public ::testing::TestWithParam<uint16_t> {};

TEST_P(BlockHeaderValMatrixVectorDim, VectorFlagAndDim) {
    const uint16_t dim = GetParam();
    std::vector<uint8_t> blob =
        BlockBuild(kBlockFile, ProcessingLevel::kLevelL3, "c", "", "", dim);
    const cortrix_block_header_t* hdr = nullptr;
    ASSERT_TRUE(BlockParse(blob.data(), blob.size(), &hdr));
    EXPECT_EQ(hdr->vector_dim, dim);
    EXPECT_EQ((hdr->flags & kFlagHasVector) != 0, dim > 0);
}

INSTANTIATE_TEST_SUITE_P(Matrix, BlockHeaderValMatrixVectorDim,
                         ::testing::Values<uint16_t>(0, 1, 384, 768, 1024,
                                                     4096, 0xFFFF));

// ==========================================================================
// block_type / level / model_id / flags_ext / enrichment field round-trips.
// ==========================================================================
struct BlockHeaderValMatrixFieldCase {
    CortrixBlockType type;
    ProcessingLevel level;
    uint32_t model_id;
    uint8_t flags_ext;
    uint8_t enrich;
    uint8_t compress;
};

class BlockHeaderValMatrixFields
    : public ::testing::TestWithParam<BlockHeaderValMatrixFieldCase> {};

TEST_P(BlockHeaderValMatrixFields, AllFieldsRoundTrip) {
    const auto& tc = GetParam();
    std::vector<uint8_t> blob =
        BlockBuild(tc.type, tc.level, "content", "meta", "chain",
                   /*vector_dim=*/8, tc.model_id, tc.flags_ext, tc.enrich,
                   tc.compress);
    const cortrix_block_header_t* hdr = nullptr;
    ASSERT_TRUE(BlockParse(blob.data(), blob.size(), &hdr));
    EXPECT_EQ(hdr->block_type, static_cast<uint16_t>(tc.type));
    EXPECT_EQ(hdr->processing_level, static_cast<uint8_t>(tc.level));
    EXPECT_EQ(hdr->embedding_model_id, tc.model_id);
    EXPECT_EQ(hdr->flags_ext, tc.flags_ext);
    EXPECT_EQ(hdr->enrichment_source, tc.enrich);
    EXPECT_EQ(hdr->compression_algo, tc.compress);
}

INSTANTIATE_TEST_SUITE_P(
    Matrix, BlockHeaderValMatrixFields,
    ::testing::Values(
        BlockHeaderValMatrixFieldCase{kBlockFile, ProcessingLevel::kLevelL0, 0,
                                      0, kEnrichNone, 0},
        BlockHeaderValMatrixFieldCase{kBlockScan, ProcessingLevel::kLevelL1, 1,
                                      kFlagExtHasEntities, kEnrichPaddleOCR, 1},
        BlockHeaderValMatrixFieldCase{kBlockDatabase, ProcessingLevel::kLevelL2,
                                      42, kFlagExtHasSummary, kEnrichDocling, 2},
        BlockHeaderValMatrixFieldCase{kBlockMemory, ProcessingLevel::kLevelL3,
                                      0xFFFFFFFFu, kFlagExtHasSparseVec,
                                      kEnrichLlm, 0},
        BlockHeaderValMatrixFieldCase{
            kBlockHypeQuestion, ProcessingLevel::kLevelL4, 1000000,
            static_cast<uint8_t>(kFlagExtHasEntities | kFlagExtHasSummary |
                                 kFlagExtHasContextualized),
            kEnrichVision, 0},
        BlockHeaderValMatrixFieldCase{kBlockDocSummary,
                                      ProcessingLevel::kLevelL4, 7, 0xFF, 4, 2}));

// ==========================================================================
// CRC integrity: tamper at representative byte offsets -> parse fails.
// (Tamper in CRC-covered ranges: header[0..79], header[84..127], payload.)
// ==========================================================================
class BlockHeaderValMatrixCrcTamper
    : public ::testing::TestWithParam<size_t> {};

TEST_P(BlockHeaderValMatrixCrcTamper, TamperBreaksParse) {
    const size_t offset = GetParam();
    std::vector<uint8_t> blob = BlockBuild(
        kBlockFile, ProcessingLevel::kLevelL3, "some content", "meta", "chain");
    ASSERT_GT(blob.size(), offset);
    blob[offset] ^= 0xFFu;  // flip bits in a CRC-covered byte
    const cortrix_block_header_t* hdr = nullptr;
    EXPECT_FALSE(BlockParse(blob.data(), blob.size(), &hdr));
}

INSTANTIATE_TEST_SUITE_P(
    Matrix, BlockHeaderValMatrixCrcTamper,
    // 0 = magic (also fails magic check); 8 = type; 79 = last pre-crc byte;
    // 84 = flags_ext (Phase-1, CRC-covered); 100 = reserved (CRC-covered);
    // 128 = first payload byte.
    ::testing::Values<size_t>(0, 4, 8, 12, 40, 79, 84, 85, 100, 127, 128, 130));

// The crc32 field itself (offsets 80..83) is NOT covered by CRC, so flipping it
// directly DOES break parse (mismatch against recomputed) — distinct path.
TEST(BlockHeaderValMatrixCrcTamper, FlippingStoredCrcBreaksParse) {
    std::vector<uint8_t> blob =
        BlockBuild(kBlockFile, ProcessingLevel::kLevelL3, "c", "", "");
    blob[80] ^= 0xFFu;
    const cortrix_block_header_t* hdr = nullptr;
    EXPECT_FALSE(BlockParse(blob.data(), blob.size(), &hdr));
}

// ==========================================================================
// BlockParse rejection matrix (truncation / null / bad magic / bad version).
// ==========================================================================
TEST(BlockHeaderValMatrixReject, NullData) {
    const cortrix_block_header_t* hdr = nullptr;
    EXPECT_FALSE(BlockParse(nullptr, 128, &hdr));
}

TEST(BlockHeaderValMatrixReject, NullHdrOut) {
    std::vector<uint8_t> blob =
        BlockBuild(kBlockFile, ProcessingLevel::kLevelL3, "c", "", "");
    EXPECT_FALSE(BlockParse(blob.data(), blob.size(), nullptr));
}

class BlockHeaderValMatrixTruncLen : public ::testing::TestWithParam<size_t> {};

TEST_P(BlockHeaderValMatrixTruncLen, ShortBufferRejected) {
    const size_t len = GetParam();
    std::vector<uint8_t> blob = BlockBuild(
        kBlockFile, ProcessingLevel::kLevelL3, "content here", "m", "c");
    ASSERT_GT(blob.size(), len);
    const cortrix_block_header_t* hdr = nullptr;
    EXPECT_FALSE(BlockParse(blob.data(), len, &hdr));
}

INSTANTIATE_TEST_SUITE_P(Matrix, BlockHeaderValMatrixTruncLen,
                         ::testing::Values<size_t>(0, 1, 64, 100, 127));

TEST(BlockHeaderValMatrixReject, BadMagic) {
    std::vector<uint8_t> blob =
        BlockBuild(kBlockFile, ProcessingLevel::kLevelL3, "c", "", "");
    auto* h = reinterpret_cast<cortrix_block_header_t*>(blob.data());
    h->magic = 0xDEADBEEFu;
    h->crc32 = BlockComputeCrc32(blob.data(), h->total_size);  // fix crc
    const cortrix_block_header_t* hdr = nullptr;
    EXPECT_FALSE(BlockParse(blob.data(), blob.size(), &hdr));
}

class BlockHeaderValMatrixBadVersion
    : public ::testing::TestWithParam<uint16_t> {};

TEST_P(BlockHeaderValMatrixBadVersion, NonV1Rejected) {
    const uint16_t ver = GetParam();
    std::vector<uint8_t> blob =
        BlockBuild(kBlockFile, ProcessingLevel::kLevelL3, "c", "", "");
    auto* h = reinterpret_cast<cortrix_block_header_t*>(blob.data());
    h->header_version = ver;
    h->crc32 = BlockComputeCrc32(blob.data(), h->total_size);  // fix crc
    const cortrix_block_header_t* hdr = nullptr;
    EXPECT_FALSE(BlockParse(blob.data(), blob.size(), &hdr));
}

INSTANTIATE_TEST_SUITE_P(Matrix, BlockHeaderValMatrixBadVersion,
                         ::testing::Values<uint16_t>(0, 2, 3, 100, 0xFFFF));

TEST(BlockHeaderValMatrixReject, TotalSizeExceedsLen) {
    std::vector<uint8_t> blob =
        BlockBuild(kBlockFile, ProcessingLevel::kLevelL3, "content", "", "");
    auto* h = reinterpret_cast<cortrix_block_header_t*>(blob.data());
    h->total_size = static_cast<uint32_t>(blob.size() + 100);  // lies
    h->crc32 = BlockComputeCrc32(blob.data(), blob.size());
    const cortrix_block_header_t* hdr = nullptr;
    EXPECT_FALSE(BlockParse(blob.data(), blob.size(), &hdr));
}

// ==========================================================================
// BlockHeaderInit baseline.
// ==========================================================================
TEST(BlockHeaderValMatrixInit, InitSetsMagicVersionSizeAndZerosRest) {
    cortrix_block_header_t hdr;
    std::memset(&hdr, 0xCD, sizeof(hdr));  // dirty
    BlockHeaderInit(&hdr);
    EXPECT_EQ(hdr.magic, kBlockMagic);
    EXPECT_EQ(hdr.header_version, kHeaderVersion);
    EXPECT_EQ(hdr.header_size, kHeaderSize);
    EXPECT_EQ(hdr.flags, 0u);
    EXPECT_EQ(hdr.flags_ext, 0u);
    EXPECT_EQ(hdr.total_size, 0u);
    for (int i = 0; i < 41; ++i) EXPECT_EQ(hdr.reserved[i], 0u);
}

// ==========================================================================
// BlockComputeCrc32 determinism + crc-field-independence.
// ==========================================================================
TEST(BlockHeaderValMatrixCrc, DeterministicAndIgnoresCrcField) {
    std::vector<uint8_t> blob =
        BlockBuild(kBlockFile, ProcessingLevel::kLevelL3, "abc", "d", "e");
    auto* h = reinterpret_cast<cortrix_block_header_t*>(blob.data());
    uint32_t c1 = BlockComputeCrc32(blob.data(), h->total_size);
    uint32_t c2 = BlockComputeCrc32(blob.data(), h->total_size);
    EXPECT_EQ(c1, c2);
    // Flipping the stored crc32 field (offset 80..83) must NOT change the
    // computed value (the field is skipped from the CRC range).
    blob[80] ^= 0xFFu;
    uint32_t c3 = BlockComputeCrc32(blob.data(), h->total_size);
    EXPECT_EQ(c1, c3);
    // Flipping a covered byte (offset 84, Phase-1 field) MUST change it.
    blob[84] ^= 0xFFu;
    uint32_t c4 = BlockComputeCrc32(blob.data(), h->total_size);
    EXPECT_NE(c1, c4);
}

// ==========================================================================
// BlockHeaderToString: null safety + content matrix.
// ==========================================================================
TEST(BlockHeaderValMatrixToString, NullHeader) {
    EXPECT_EQ(BlockHeaderToString(nullptr), "BlockHeader{null}");
}

struct BlockHeaderValMatrixToStringCase {
    CortrixBlockType type;
    uint8_t enrich;
    std::string type_token;
    std::string enrich_token;
};

class BlockHeaderValMatrixToString
    : public ::testing::TestWithParam<BlockHeaderValMatrixToStringCase> {};

TEST_P(BlockHeaderValMatrixToString, ContainsTypeAndEnrichTokens) {
    const auto& tc = GetParam();
    std::vector<uint8_t> blob =
        BlockBuild(tc.type, ProcessingLevel::kLevelL3, "c", "", "",
                   /*vector_dim=*/0, /*model_id=*/0, /*flags_ext=*/0, tc.enrich);
    const cortrix_block_header_t* hdr = nullptr;
    ASSERT_TRUE(BlockParse(blob.data(), blob.size(), &hdr));
    std::string s = BlockHeaderToString(hdr);
    EXPECT_NE(s.find("v=1"), std::string::npos);
    EXPECT_NE(s.find(tc.type_token), std::string::npos) << s;
    EXPECT_NE(s.find(tc.enrich_token), std::string::npos) << s;
}

INSTANTIATE_TEST_SUITE_P(
    Matrix, BlockHeaderValMatrixToString,
    ::testing::Values(
        BlockHeaderValMatrixToStringCase{kBlockFile, kEnrichNone, "FILE",
                                         "none"},
        BlockHeaderValMatrixToStringCase{kBlockScan, kEnrichPaddleOCR, "SCAN",
                                         "PaddleOCR"},
        BlockHeaderValMatrixToStringCase{kBlockDatabase, kEnrichDocling,
                                         "DATABASE", "Docling"},
        BlockHeaderValMatrixToStringCase{kBlockMemory, kEnrichLlm, "MEMORY",
                                         "LLM"},
        BlockHeaderValMatrixToStringCase{kBlockMeta, kEnrichVision, "META",
                                         "Vision"}));

}  // namespace
}  // namespace cortrix
