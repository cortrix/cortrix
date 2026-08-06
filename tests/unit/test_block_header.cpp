#include <gtest/gtest.h>
#include "cortrix/common/block_header.h"
#include <cstring>
#include <openssl/evp.h>

using namespace cortrix;

// Test 1: Static assert already in header; verify header struct size
TEST(BlockHeaderTest, StructSize128) {
    EXPECT_EQ(sizeof(cortrix_block_header_t), 128u);
}

// Test 2: Init fills magic, version, header_size and zeros rest
TEST(BlockHeaderTest, InitFields) {
    cortrix_block_header_t hdr;
    std::memset(&hdr, 0xFF, sizeof(hdr));  // fill with garbage
    BlockHeaderInit(&hdr);

    EXPECT_EQ(hdr.magic, kBlockMagic);
    EXPECT_EQ(hdr.header_version, kHeaderVersion);
    EXPECT_EQ(hdr.header_size, kHeaderSize);
    EXPECT_EQ(hdr.block_type, 0);
    EXPECT_EQ(hdr.processing_level, 0);
    EXPECT_EQ(hdr.flags, 0);
    EXPECT_EQ(hdr.content_offset, 0u);
    EXPECT_EQ(hdr.crc32, 0u);
    // Reserved should be zeroed (block header shrank reserved 44B → 41B; use sizeof so
    // this stays correct across layout changes).
    for (size_t i = 0; i < sizeof(hdr.reserved); ++i) {
        EXPECT_EQ(hdr.reserved[i], 0) << "reserved[" << i << "] not zero";
    }
    // Block header fields are also zeroed by Init.
    EXPECT_EQ(hdr.flags_ext, 0);
    EXPECT_EQ(hdr.enrichment_source, 0);
    EXPECT_EQ(hdr.compression_algo, 0);
}

// Test 3: Build and Parse roundtrip
TEST(BlockHeaderTest, BuildAndParseRoundtrip) {
    std::string content = "Hello, this is test content for block header.";
    std::string metadata = R"({"doc_id":42,"type":"file"})";
    std::string chain = "parse->chunk->embed";

    auto blob = BlockBuild(CortrixBlockType::kBlockFile,
                            ProcessingLevel::kLevelL3,
                            content, metadata, chain,
                            1024, 1);

    ASSERT_GE(blob.size(), 128u);
    EXPECT_EQ(blob.size(), 128 + content.size() + metadata.size() + chain.size());

    const cortrix_block_header_t* hdr = nullptr;
    ASSERT_TRUE(BlockParse(blob.data(), blob.size(), &hdr));
    ASSERT_NE(hdr, nullptr);

    EXPECT_EQ(hdr->magic, kBlockMagic);
    EXPECT_EQ(hdr->header_version, kHeaderVersion);
    EXPECT_EQ(hdr->block_type, static_cast<uint16_t>(CortrixBlockType::kBlockFile));
    EXPECT_EQ(hdr->processing_level, static_cast<uint8_t>(ProcessingLevel::kLevelL3));
    EXPECT_EQ(hdr->content_length, static_cast<uint32_t>(content.size()));
    EXPECT_EQ(hdr->metadata_length, static_cast<uint32_t>(metadata.size()));
    EXPECT_EQ(hdr->chain_length, static_cast<uint32_t>(chain.size()));
    EXPECT_EQ(hdr->vector_dim, 1024);
    EXPECT_EQ(hdr->embedding_model_id, 1u);
    EXPECT_EQ(hdr->total_size, static_cast<uint32_t>(blob.size()));

    // Verify payload content using design read formula: blob_data + 128 + offset
    std::string parsed_content(reinterpret_cast<const char*>(blob.data() + kHeaderSize + hdr->content_offset),
                               hdr->content_length);
    EXPECT_EQ(parsed_content, content);

    std::string parsed_metadata(reinterpret_cast<const char*>(blob.data() + kHeaderSize + hdr->metadata_offset),
                                hdr->metadata_length);
    EXPECT_EQ(parsed_metadata, metadata);
}

// Test 4: CRC32 tamper detection
TEST(BlockHeaderTest, Crc32TamperDetection) {
    auto blob = BlockBuild(CortrixBlockType::kBlockFile,
                            ProcessingLevel::kLevelL1,
                            "test content", "{}", "");

    const cortrix_block_header_t* hdr = nullptr;
    ASSERT_TRUE(BlockParse(blob.data(), blob.size(), &hdr));

    // Tamper with payload
    blob[130] ^= 0xFF;
    EXPECT_FALSE(BlockParse(blob.data(), blob.size(), &hdr));
}

// Test 5: Wrong magic fails parse
TEST(BlockHeaderTest, WrongMagicFails) {
    auto blob = BlockBuild(CortrixBlockType::kBlockFile,
                            ProcessingLevel::kLevelL0,
                            "x", "", "");

    // Corrupt magic bytes
    blob[0] = 0xFF;
    const cortrix_block_header_t* hdr = nullptr;
    EXPECT_FALSE(BlockParse(blob.data(), blob.size(), &hdr));
}

// Test 6: Empty payload roundtrip
TEST(BlockHeaderTest, EmptyPayload) {
    auto blob = BlockBuild(CortrixBlockType::kBlockFile,
                            ProcessingLevel::kLevelL0,
                            "", "", "");

    EXPECT_EQ(blob.size(), 128u);

    const cortrix_block_header_t* hdr = nullptr;
    ASSERT_TRUE(BlockParse(blob.data(), blob.size(), &hdr));
    EXPECT_EQ(hdr->content_length, 0u);
    EXPECT_EQ(hdr->metadata_length, 0u);
    EXPECT_EQ(hdr->chain_length, 0u);
    EXPECT_EQ(hdr->flags & kFlagHasContent, 0);
    EXPECT_EQ(hdr->flags & kFlagHasMetadata, 0);
}

// Test 7: All block types roundtrip
TEST(BlockHeaderTest, AllBlockTypes) {
    CortrixBlockType types[] = {
        CortrixBlockType::kBlockFile,
        CortrixBlockType::kBlockScan,
        CortrixBlockType::kBlockDatabase,
        CortrixBlockType::kBlockAudio,
        CortrixBlockType::kBlockVideo,
        CortrixBlockType::kBlockImage,
        CortrixBlockType::kBlockMemory,
    };

    for (auto bt : types) {
        auto blob = BlockBuild(bt, ProcessingLevel::kLevelL0, "data", "", "");
        const cortrix_block_header_t* hdr = nullptr;
        ASSERT_TRUE(BlockParse(blob.data(), blob.size(), &hdr))
            << "Failed for block type " << static_cast<int>(bt);
        EXPECT_EQ(hdr->block_type, static_cast<uint16_t>(bt));
    }
}

// Test 8: Flags computation
TEST(BlockHeaderTest, FlagsComputation) {
    // Content only
    {
        auto blob = BlockBuild(CortrixBlockType::kBlockFile, ProcessingLevel::kLevelL0,
                                "content", "", "");
        const cortrix_block_header_t* hdr = nullptr;
        ASSERT_TRUE(BlockParse(blob.data(), blob.size(), &hdr));
        EXPECT_NE(hdr->flags & kFlagHasContent, 0);
        EXPECT_EQ(hdr->flags & kFlagHasMetadata, 0);
        EXPECT_EQ(hdr->flags & kFlagHasChain, 0);
        EXPECT_EQ(hdr->flags & kFlagHasVector, 0);
    }
    // With vector dim
    {
        auto blob = BlockBuild(CortrixBlockType::kBlockFile, ProcessingLevel::kLevelL3,
                                "c", "{}", "chain", 1024, 1);
        const cortrix_block_header_t* hdr = nullptr;
        ASSERT_TRUE(BlockParse(blob.data(), blob.size(), &hdr));
        EXPECT_NE(hdr->flags & kFlagHasContent, 0);
        EXPECT_NE(hdr->flags & kFlagHasMetadata, 0);
        EXPECT_NE(hdr->flags & kFlagHasChain, 0);
        EXPECT_NE(hdr->flags & kFlagHasVector, 0);
    }
}

// Test 9: Timestamps are non-zero
TEST(BlockHeaderTest, TimestampsNonZero) {
    auto blob = BlockBuild(CortrixBlockType::kBlockFile, ProcessingLevel::kLevelL0,
                            "x", "", "");
    const cortrix_block_header_t* hdr = nullptr;
    ASSERT_TRUE(BlockParse(blob.data(), blob.size(), &hdr));
    EXPECT_GT(hdr->created_at_us, 0u);
    EXPECT_GT(hdr->updated_at_us, 0u);
    EXPECT_EQ(hdr->created_at_us, hdr->updated_at_us);
}

// Test 10: Null/insufficient data fails parse
TEST(BlockHeaderTest, NullDataFails) {
    const cortrix_block_header_t* hdr = nullptr;
    EXPECT_FALSE(BlockParse(nullptr, 128, &hdr));
    EXPECT_FALSE(BlockParse("x", 1, &hdr));  // too short

    uint8_t small[64] = {};
    EXPECT_FALSE(BlockParse(small, 64, &hdr));  // less than header size
}

// Test 11: Null output pointer fails parse
TEST(BlockHeaderTest, NullOutputPointerFails) {
    auto blob = BlockBuild(CortrixBlockType::kBlockFile, ProcessingLevel::kLevelL0,
                            "x", "", "");
    EXPECT_FALSE(BlockParse(blob.data(), blob.size(), nullptr));
}

// Test 12: Wrong header version fails parse
TEST(BlockHeaderTest, WrongVersionFails) {
    auto blob = BlockBuild(CortrixBlockType::kBlockFile, ProcessingLevel::kLevelL0,
                            "x", "", "");
    // Corrupt version field (offset 4-5 in packed struct)
    auto* hdr = reinterpret_cast<cortrix_block_header_t*>(blob.data());
    hdr->header_version = 99;
    // Recompute CRC to isolate version check
    hdr->crc32 = BlockComputeCrc32(blob.data(), blob.size());

    const cortrix_block_header_t* parsed = nullptr;
    EXPECT_FALSE(BlockParse(blob.data(), blob.size(), &parsed));
}

// Test 13: total_size larger than buffer fails parse
TEST(BlockHeaderTest, TotalSizeLargerThanBufferFails) {
    auto blob = BlockBuild(CortrixBlockType::kBlockFile, ProcessingLevel::kLevelL0,
                            "x", "", "");
    auto* hdr = reinterpret_cast<cortrix_block_header_t*>(blob.data());
    hdr->total_size = static_cast<uint32_t>(blob.size() + 100);  // claim larger
    // Recompute CRC for the modified header
    hdr->crc32 = BlockComputeCrc32(blob.data(), hdr->total_size);

    const cortrix_block_header_t* parsed = nullptr;
    // Should fail because total_size > len
    EXPECT_FALSE(BlockParse(blob.data(), blob.size(), &parsed));
}

// Test 14: Large payload roundtrip (stress test payload offsets)
TEST(BlockHeaderTest, LargePayloadRoundtrip) {
    std::string big_content(10000, 'A');
    std::string big_meta(5000, 'M');
    std::string big_chain(3000, 'C');

    auto blob = BlockBuild(CortrixBlockType::kBlockFile, ProcessingLevel::kLevelL3,
                            big_content, big_meta, big_chain, 1024, 7);

    EXPECT_EQ(blob.size(), 128 + 10000 + 5000 + 3000);

    const cortrix_block_header_t* hdr = nullptr;
    ASSERT_TRUE(BlockParse(blob.data(), blob.size(), &hdr));
    EXPECT_EQ(hdr->content_length, 10000u);
    EXPECT_EQ(hdr->metadata_length, 5000u);
    EXPECT_EQ(hdr->chain_length, 3000u);
    EXPECT_EQ(hdr->total_size, static_cast<uint32_t>(blob.size()));

    // Verify content payload using design read formula: blob_data + 128 + offset
    std::string parsed_content(reinterpret_cast<const char*>(blob.data() + kHeaderSize + hdr->content_offset),
                               hdr->content_length);
    EXPECT_EQ(parsed_content, big_content);

    std::string parsed_meta(reinterpret_cast<const char*>(blob.data() + kHeaderSize + hdr->metadata_offset),
                            hdr->metadata_length);
    EXPECT_EQ(parsed_meta, big_meta);

    std::string parsed_chain(reinterpret_cast<const char*>(blob.data() + kHeaderSize + hdr->chain_offset),
                             hdr->chain_length);
    EXPECT_EQ(parsed_chain, big_chain);
}

// Test 15: All processing levels roundtrip
TEST(BlockHeaderTest, AllProcessingLevels) {
    ProcessingLevel levels[] = {
        ProcessingLevel::kLevelL0,
        ProcessingLevel::kLevelL1,
        ProcessingLevel::kLevelL2,
        ProcessingLevel::kLevelL3,
    };

    for (auto lv : levels) {
        auto blob = BlockBuild(CortrixBlockType::kBlockFile, lv, "data", "", "");
        const cortrix_block_header_t* hdr = nullptr;
        ASSERT_TRUE(BlockParse(blob.data(), blob.size(), &hdr))
            << "Failed for processing level " << static_cast<int>(lv);
        EXPECT_EQ(hdr->processing_level, static_cast<uint8_t>(lv));
    }
}

// Test 16: Metadata-only payload (content empty, metadata present)
TEST(BlockHeaderTest, MetadataOnlyPayload) {
    auto blob = BlockBuild(CortrixBlockType::kBlockFile, ProcessingLevel::kLevelL0,
                            "", R"({"key":"value"})", "");

    const cortrix_block_header_t* hdr = nullptr;
    ASSERT_TRUE(BlockParse(blob.data(), blob.size(), &hdr));
    EXPECT_EQ(hdr->content_length, 0u);
    EXPECT_GT(hdr->metadata_length, 0u);
    EXPECT_EQ(hdr->flags & kFlagHasContent, 0);
    EXPECT_NE(hdr->flags & kFlagHasMetadata, 0);
}

// Test 17: CRC32 is deterministic for same input
TEST(BlockHeaderTest, Crc32Deterministic) {
    auto blob1 = BlockBuild(CortrixBlockType::kBlockFile, ProcessingLevel::kLevelL0,
                             "test", "{}", "chain");
    auto blob2 = BlockBuild(CortrixBlockType::kBlockFile, ProcessingLevel::kLevelL0,
                             "test", "{}", "chain");

    // CRC32 should be the same for same data (timestamps differ, but CRC
    // covers header[0..79] which includes timestamps at offset 56-71.
    // Since builds happen at different times, CRC will differ.
    // Instead, verify CRC is consistent for a single blob.
    uint32_t crc1 = BlockComputeCrc32(blob1.data(), blob1.size());
    uint32_t crc2 = BlockComputeCrc32(blob1.data(), blob1.size());
    EXPECT_EQ(crc1, crc2);
}

// Test 18: Payload offset alignment (relative to header end)
TEST(BlockHeaderTest, PayloadOffsetsCorrect) {
    std::string content = "ABCDE";
    std::string meta = "12345";
    std::string chain = "xyz";

    auto blob = BlockBuild(CortrixBlockType::kBlockFile, ProcessingLevel::kLevelL0,
                            content, meta, chain);

    const cortrix_block_header_t* hdr = nullptr;
    ASSERT_TRUE(BlockParse(blob.data(), blob.size(), &hdr));

    // Offsets are relative to header end (byte 128)
    // Content starts right after header → relative offset 0
    EXPECT_EQ(hdr->content_offset, 0u);
    // Metadata starts after content → relative offset 5
    EXPECT_EQ(hdr->metadata_offset, 5u);
    // Chain starts after metadata → relative offset 10
    EXPECT_EQ(hdr->chain_offset, 10u);

    // Verify actual data read using design's read formula: blob_data + 128 + offset
    std::string parsed_content(reinterpret_cast<const char*>(blob.data() + 128 + hdr->content_offset),
                               hdr->content_length);
    EXPECT_EQ(parsed_content, content);

    std::string parsed_meta(reinterpret_cast<const char*>(blob.data() + 128 + hdr->metadata_offset),
                            hdr->metadata_length);
    EXPECT_EQ(parsed_meta, meta);

    std::string parsed_chain(reinterpret_cast<const char*>(blob.data() + 128 + hdr->chain_offset),
                             hdr->chain_length);
    EXPECT_EQ(parsed_chain, chain);
}

// ============================================================
// Design Alignment Tests (Review index+reranker)
// ============================================================

// Test 19: Relative offsets are zero-based from header end
TEST(BlockHeaderTest, RelativeOffsetsFromHeaderEnd) {
    // Single payload section: content only
    auto blob = BlockBuild(CortrixBlockType::kBlockFile, ProcessingLevel::kLevelL2,
                            "hello", "", "");

    const cortrix_block_header_t* hdr = nullptr;
    ASSERT_TRUE(BlockParse(blob.data(), blob.size(), &hdr));

    // Content should start at relative offset 0 from header end
    EXPECT_EQ(hdr->content_offset, 0u);
    EXPECT_EQ(hdr->content_length, 5u);

    // Read using design formula: blob_data + kHeaderSize + content_offset
    std::string read_content(reinterpret_cast<const char*>(blob.data() + kHeaderSize + hdr->content_offset),
                             hdr->content_length);
    EXPECT_EQ(read_content, "hello");
}

// Test 20: kFlagCompressed exists and has correct value
TEST(BlockHeaderTest, FlagCompressedDefined) {
    // Design: CORTRIX_BF_COMPRESSED = (1 << 5) = 0x20
    EXPECT_EQ(kFlagCompressed, 0x20);
    // Verify it doesn't overlap with other flags
    EXPECT_EQ(kFlagHasContent & kFlagCompressed, 0);
    EXPECT_EQ(kFlagHasVector & kFlagCompressed, 0);
    EXPECT_EQ(kFlagHasFts & kFlagCompressed, 0);
    EXPECT_EQ(kFlagHasMetadata & kFlagCompressed, 0);
    EXPECT_EQ(kFlagHasChain & kFlagCompressed, 0);
}

// Test 21: kFlagHasFts value matches design bit position
TEST(BlockHeaderTest, FlagHasFtsValue) {
    // Design: CORTRIX_BF_HAS_FTS = (1 << 2) = 0x04
    EXPECT_EQ(kFlagHasFts, 0x04);
}

// Test 22: Content-only payload has offset 0
TEST(BlockHeaderTest, ContentOnlyOffsetZero) {
    auto blob = BlockBuild(CortrixBlockType::kBlockFile, ProcessingLevel::kLevelL2,
                            "data", "", "");
    const cortrix_block_header_t* hdr = nullptr;
    ASSERT_TRUE(BlockParse(blob.data(), blob.size(), &hdr));
    EXPECT_EQ(hdr->content_offset, 0u);
    // metadata and chain should remain 0 (not set)
    EXPECT_EQ(hdr->metadata_offset, 0u);
    EXPECT_EQ(hdr->metadata_length, 0u);
    EXPECT_EQ(hdr->chain_offset, 0u);
    EXPECT_EQ(hdr->chain_length, 0u);
}

// Test 23: ProcessingLevel enum values match design
TEST(BlockHeaderTest, ProcessingLevelValues) {
    // Design: L0=0 skip, L1=1 metadata, L2=2 BM25+meta, L3=3 vec+BM25+meta
    EXPECT_EQ(static_cast<uint8_t>(ProcessingLevel::kLevelL0), 0);
    EXPECT_EQ(static_cast<uint8_t>(ProcessingLevel::kLevelL1), 1);
    EXPECT_EQ(static_cast<uint8_t>(ProcessingLevel::kLevelL2), 2);
    EXPECT_EQ(static_cast<uint8_t>(ProcessingLevel::kLevelL3), 3);
}

// Test 24: CortrixBlockType enum values match design
TEST(BlockHeaderTest, BlockTypeValues) {
    // Design: FILE=1, SCAN=2, DATABASE=3, AUDIO=4, VIDEO=5, IMAGE=6, MEMORY=7
    EXPECT_EQ(static_cast<uint16_t>(CortrixBlockType::kBlockFile), 1);
    EXPECT_EQ(static_cast<uint16_t>(CortrixBlockType::kBlockScan), 2);
    EXPECT_EQ(static_cast<uint16_t>(CortrixBlockType::kBlockDatabase), 3);
    EXPECT_EQ(static_cast<uint16_t>(CortrixBlockType::kBlockAudio), 4);
    EXPECT_EQ(static_cast<uint16_t>(CortrixBlockType::kBlockVideo), 5);
    EXPECT_EQ(static_cast<uint16_t>(CortrixBlockType::kBlockImage), 6);
    EXPECT_EQ(static_cast<uint16_t>(CortrixBlockType::kBlockMemory), 7);
}

// ============================================================
// Design Alignment Tests (index+reranker Final Review)
// ============================================================

// Test 25: kFlagHasFts is set for L2 blocks with content (design)
TEST(BlockHeaderTest, FlagHasFtsSetForL2WithContent) {
    auto buf = BlockBuild(
        CortrixBlockType::kBlockFile,
        ProcessingLevel::kLevelL2,
        "some content text",
        "",   // no metadata
        "",   // no chain
        0,    // no vector
        0);

    const cortrix_block_header_t* hdr = nullptr;
    ASSERT_TRUE(BlockParse(buf.data(), buf.size(), &hdr));
    EXPECT_TRUE(hdr->flags & kFlagHasFts);
    EXPECT_TRUE(hdr->flags & kFlagHasContent);
}

// Test 26: kFlagHasFts is set for L3 blocks with content
TEST(BlockHeaderTest, FlagHasFtsSetForL3WithContent) {
    auto buf = BlockBuild(
        CortrixBlockType::kBlockFile,
        ProcessingLevel::kLevelL3,
        "L3 content text",
        "",   // no metadata
        "",   // no chain
        128,  // has vector dim
        1);

    const cortrix_block_header_t* hdr = nullptr;
    ASSERT_TRUE(BlockParse(buf.data(), buf.size(), &hdr));
    EXPECT_TRUE(hdr->flags & kFlagHasFts);
    EXPECT_TRUE(hdr->flags & kFlagHasContent);
    EXPECT_TRUE(hdr->flags & kFlagHasVector);
}

// Test 27: kFlagHasFts is NOT set for L1 blocks (metadata only)
TEST(BlockHeaderTest, FlagHasFtsNotSetForL1) {
    auto buf = BlockBuild(
        CortrixBlockType::kBlockFile,
        ProcessingLevel::kLevelL1,
        "L1 content",
        "",
        "",
        0,
        0);

    const cortrix_block_header_t* hdr = nullptr;
    ASSERT_TRUE(BlockParse(buf.data(), buf.size(), &hdr));
    EXPECT_FALSE(hdr->flags & kFlagHasFts);
}

// Test 28: kFlagHasFts is NOT set for L0 blocks
TEST(BlockHeaderTest, FlagHasFtsNotSetForL0) {
    auto buf = BlockBuild(
        CortrixBlockType::kBlockFile,
        ProcessingLevel::kLevelL0,
        "L0 content",
        "",
        "",
        0,
        0);

    const cortrix_block_header_t* hdr = nullptr;
    ASSERT_TRUE(BlockParse(buf.data(), buf.size(), &hdr));
    EXPECT_FALSE(hdr->flags & kFlagHasFts);
}

// Test 29: kFlagHasFts is NOT set for L2 without content
TEST(BlockHeaderTest, FlagHasFtsNotSetForL2WithoutContent) {
    auto buf = BlockBuild(
        CortrixBlockType::kBlockFile,
        ProcessingLevel::kLevelL2,
        "",   // no content
        "{\"key\":\"val\"}",
        "",
        0,
        0);

    const cortrix_block_header_t* hdr = nullptr;
    ASSERT_TRUE(BlockParse(buf.data(), buf.size(), &hdr));
    EXPECT_FALSE(hdr->flags & kFlagHasFts);
}

// Test 30: Content hash is computed correctly (SHA-256 first 16 bytes)
TEST(BlockHeaderTest, ContentHashComputedCorrectly) {
    std::string content = "hello world";

    auto buf = BlockBuild(
        CortrixBlockType::kBlockFile,
        ProcessingLevel::kLevelL2,
        content,
        "",
        "",
        0,
        0);

    const cortrix_block_header_t* hdr = nullptr;
    ASSERT_TRUE(BlockParse(buf.data(), buf.size(), &hdr));

    // Compute expected SHA-256 independently
    unsigned char expected_sha256[32] = {};
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    ASSERT_NE(ctx, nullptr);
    ASSERT_EQ(EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr), 1);
    ASSERT_EQ(EVP_DigestUpdate(ctx, content.data(), content.size()), 1);
    unsigned int digest_len = 0;
    EVP_DigestFinal_ex(ctx, expected_sha256, &digest_len);
    EVP_MD_CTX_free(ctx);

    uint64_t expected_high, expected_low;
    std::memcpy(&expected_high, expected_sha256, 8);
    std::memcpy(&expected_low, expected_sha256 + 8, 8);

    EXPECT_EQ(hdr->content_hash_high, expected_high);
    EXPECT_EQ(hdr->content_hash_low, expected_low);
}

// Test 31: Content hash is zero when no content
TEST(BlockHeaderTest, ContentHashZeroWhenNoContent) {
    auto buf = BlockBuild(
        CortrixBlockType::kBlockFile,
        ProcessingLevel::kLevelL1,
        "",   // no content
        "{\"key\":\"val\"}",
        "",
        0,
        0);

    const cortrix_block_header_t* hdr = nullptr;
    ASSERT_TRUE(BlockParse(buf.data(), buf.size(), &hdr));

    EXPECT_EQ(hdr->content_hash_high, 0u);
    EXPECT_EQ(hdr->content_hash_low, 0u);
}

// Test 32: Different content produces different hash
TEST(BlockHeaderTest, DifferentContentDifferentHash) {
    auto buf1 = BlockBuild(
        CortrixBlockType::kBlockFile,
        ProcessingLevel::kLevelL2,
        "content alpha",
        "",
        "",
        0,
        0);

    auto buf2 = BlockBuild(
        CortrixBlockType::kBlockFile,
        ProcessingLevel::kLevelL2,
        "content beta",
        "",
        "",
        0,
        0);

    const cortrix_block_header_t* hdr1 = nullptr;
    const cortrix_block_header_t* hdr2 = nullptr;
    ASSERT_TRUE(BlockParse(buf1.data(), buf1.size(), &hdr1));
    ASSERT_TRUE(BlockParse(buf2.data(), buf2.size(), &hdr2));

    // At least one of the two halves must differ
    EXPECT_TRUE(hdr1->content_hash_high != hdr2->content_hash_high ||
                hdr1->content_hash_low != hdr2->content_hash_low);
}

// ============================================================================
// Block header Phase 1 — new fields (flags_ext / enrichment_source / compression_algo),
// extended enums (META=8, L4), CRC range 84-127, magic CRTX, ToString.
// (Design tests #33-48.)
// ============================================================================

TEST(BlockHeaderTest, BLOCKFRAMEWORK_StructSize128) {
    EXPECT_EQ(sizeof(cortrix_block_header_t), 128u);
}

TEST(BlockHeaderTest, BLOCKFRAMEWORK_MagicIsCrtx) {
    EXPECT_EQ(kBlockMagic, 0x43525458u);  // "CRTX"
    auto blob = BlockBuild(kBlockFile, kLevelL1, "x", "", "");
    const cortrix_block_header_t* hdr = nullptr;
    ASSERT_TRUE(BlockParse(blob.data(), blob.size(), &hdr));
    EXPECT_EQ(hdr->magic, 0x43525458u);
}

TEST(BlockHeaderTest, BLOCKFRAMEWORK_InitSetsVersion1) {
    cortrix_block_header_t hdr;
    BlockHeaderInit(&hdr);
    EXPECT_EQ(hdr.header_version, 1u);
    EXPECT_EQ(hdr.magic, kBlockMagic);
    EXPECT_EQ(hdr.header_size, 128u);
}

TEST(BlockHeaderTest, BLOCKFRAMEWORK_BuildParseRoundtripAllFields) {
    auto blob = BlockBuild(kBlockMeta, kLevelL4, "content", "{\"k\":1}", "chain",
                           /*vector_dim=*/768, /*embedding_model_id=*/42,
                           /*flags_ext=*/kFlagExtHasEntities | kFlagExtHasSummary,
                           /*enrichment_source=*/kEnrichLlm,
                           /*compression_algo=*/1);
    const cortrix_block_header_t* hdr = nullptr;
    ASSERT_TRUE(BlockParse(blob.data(), blob.size(), &hdr));
    EXPECT_EQ(hdr->block_type, kBlockMeta);
    EXPECT_EQ(hdr->processing_level, kLevelL4);
    EXPECT_EQ(hdr->vector_dim, 768u);
    EXPECT_EQ(hdr->embedding_model_id, 42u);
    EXPECT_EQ(hdr->flags_ext, kFlagExtHasEntities | kFlagExtHasSummary);
    EXPECT_EQ(hdr->enrichment_source, kEnrichLlm);
    EXPECT_EQ(hdr->compression_algo, 1u);
}

TEST(BlockHeaderTest, BLOCKFRAMEWORK_FlagsExtHasEntities) {
    auto blob = BlockBuild(kBlockFile, kLevelL3, "c", "", "", 0, 0,
                           kFlagExtHasEntities);
    const cortrix_block_header_t* hdr = nullptr;
    ASSERT_TRUE(BlockParse(blob.data(), blob.size(), &hdr));
    EXPECT_TRUE(hdr->flags_ext & kFlagExtHasEntities);
    EXPECT_FALSE(hdr->flags_ext & kFlagExtHasSummary);
}

TEST(BlockHeaderTest, BLOCKFRAMEWORK_FlagsExtHasSummary) {
    auto blob = BlockBuild(kBlockFile, kLevelL3, "c", "", "", 0, 0,
                           kFlagExtHasSummary);
    const cortrix_block_header_t* hdr = nullptr;
    ASSERT_TRUE(BlockParse(blob.data(), blob.size(), &hdr));
    EXPECT_TRUE(hdr->flags_ext & kFlagExtHasSummary);
}

TEST(BlockHeaderTest, BLOCKFRAMEWORK_FlagsExtCombined) {
    const uint8_t fx = kFlagExtHasEntities | kFlagExtHasSummary |
                       kFlagExtIsAnomalous | kFlagExtHasContextualized |
                       kFlagExtHasSparseVec;
    auto blob = BlockBuild(kBlockFile, kLevelL3, "c", "", "", 0, 0, fx);
    const cortrix_block_header_t* hdr = nullptr;
    ASSERT_TRUE(BlockParse(blob.data(), blob.size(), &hdr));
    EXPECT_EQ(hdr->flags_ext, fx);
}

TEST(BlockHeaderTest, BLOCKFRAMEWORK_EnrichmentSources) {
    for (uint8_t src : {kEnrichNone, kEnrichDocling, kEnrichPaddleOCR,
                        kEnrichLlm, kEnrichVision}) {
        auto blob = BlockBuild(kBlockFile, kLevelL2, "c", "", "", 0, 0, 0, src);
        const cortrix_block_header_t* hdr = nullptr;
        ASSERT_TRUE(BlockParse(blob.data(), blob.size(), &hdr));
        EXPECT_EQ(hdr->enrichment_source, src);
    }
}

TEST(BlockHeaderTest, BLOCKFRAMEWORK_CompressionAlgoPlaceholder) {
    auto blob = BlockBuild(kBlockFile, kLevelL2, "c", "", "", 0, 0, 0, 0,
                           /*compression_algo=*/2);
    const cortrix_block_header_t* hdr = nullptr;
    ASSERT_TRUE(BlockParse(blob.data(), blob.size(), &hdr));
    EXPECT_EQ(hdr->compression_algo, 2u);
}

TEST(BlockHeaderTest, BLOCKFRAMEWORK_BlockTypeMeta) {
    auto blob = BlockBuild(kBlockMeta, kLevelL1, "m", "", "");
    const cortrix_block_header_t* hdr = nullptr;
    ASSERT_TRUE(BlockParse(blob.data(), blob.size(), &hdr));
    EXPECT_EQ(hdr->block_type, 8u);
}

TEST(BlockHeaderTest, BLOCKFRAMEWORK_ProcessingLevelL4) {
    auto blob = BlockBuild(kBlockFile, kLevelL4, "c", "", "");
    const cortrix_block_header_t* hdr = nullptr;
    ASSERT_TRUE(BlockParse(blob.data(), blob.size(), &hdr));
    EXPECT_EQ(hdr->processing_level, 4u);
}

// CRC now covers offset 84 (flags_ext) — tampering it must fail parse.
TEST(BlockHeaderTest, BLOCKFRAMEWORK_CrcCoversExtFields) {
    auto blob = BlockBuild(kBlockFile, kLevelL3, "content", "", "", 0, 0,
                           kFlagExtHasEntities);
    const cortrix_block_header_t* hdr = nullptr;
    ASSERT_TRUE(BlockParse(blob.data(), blob.size(), &hdr));
    blob[84] ^= 0xFF;  // flip flags_ext byte
    EXPECT_FALSE(BlockParse(blob.data(), blob.size(), &hdr))
        << "CRC must cover the flags_ext field (offset 84)";
}

// CRC now covers reserved (offset 87-127) — tampering it must fail parse.
TEST(BlockHeaderTest, BLOCKFRAMEWORK_CrcCoversReserved) {
    auto blob = BlockBuild(kBlockFile, kLevelL3, "content", "", "");
    const cortrix_block_header_t* hdr = nullptr;
    ASSERT_TRUE(BlockParse(blob.data(), blob.size(), &hdr));
    blob[100] ^= 0xFF;  // flip a reserved byte
    EXPECT_FALSE(BlockParse(blob.data(), blob.size(), &hdr))
        << "CRC must cover reserved bytes (offset 87-127)";
}

// Flipping the crc32 field's own bytes (80-83) is still detected (value changes).
TEST(BlockHeaderTest, BLOCKFRAMEWORK_CrcFieldItselfNotInRangeButTamperDetected) {
    auto blob = BlockBuild(kBlockFile, kLevelL3, "content", "", "");
    const cortrix_block_header_t* hdr = nullptr;
    ASSERT_TRUE(BlockParse(blob.data(), blob.size(), &hdr));
    blob[80] ^= 0xFF;  // corrupt stored crc → mismatch vs recomputed
    EXPECT_FALSE(BlockParse(blob.data(), blob.size(), &hdr));
}

TEST(BlockHeaderTest, BLOCKFRAMEWORK_HeaderToString) {
    auto blob = BlockBuild(kBlockFile, kLevelL4, "content", "", "", 0, 0,
                           kFlagExtHasEntities, kEnrichLlm);
    const cortrix_block_header_t* hdr = nullptr;
    ASSERT_TRUE(BlockParse(blob.data(), blob.size(), &hdr));
    std::string s = BlockHeaderToString(hdr);
    EXPECT_NE(s.find("v=1"), std::string::npos);
    EXPECT_NE(s.find("type=FILE"), std::string::npos);
    EXPECT_NE(s.find("level=L4"), std::string::npos);
    EXPECT_NE(s.find("enrich=LLM"), std::string::npos);
}

TEST(BlockHeaderTest, BLOCKFRAMEWORK_HeaderToStringNull) {
    EXPECT_EQ(BlockHeaderToString(nullptr), "BlockHeader{null}");
}

// header_version != 1 is rejected (no V1/V2 compat in Phase 1).
TEST(BlockHeaderTest, BLOCKFRAMEWORK_UnsupportedVersionRejected) {
    auto blob = BlockBuild(kBlockFile, kLevelL1, "content", "", "");
    // Bump version to 2 and recompute CRC so only the version gate can reject it.
    auto* hdr = reinterpret_cast<cortrix_block_header_t*>(blob.data());
    hdr->header_version = 2;
    hdr->crc32 = BlockComputeCrc32(blob.data(), hdr->total_size);
    const cortrix_block_header_t* parsed = nullptr;
    EXPECT_FALSE(BlockParse(blob.data(), blob.size(), &parsed))
        << "header_version=2 must be rejected even with a valid CRC";
}

// MVP callers (no block header params) still build/parse cleanly — defaults all zero.
TEST(BlockHeaderTest, BLOCKFRAMEWORK_MvpCallerDefaultsZero) {
    auto blob = BlockBuild(kBlockFile, kLevelL2, "legacy", "{}", "");
    const cortrix_block_header_t* hdr = nullptr;
    ASSERT_TRUE(BlockParse(blob.data(), blob.size(), &hdr));
    EXPECT_EQ(hdr->flags_ext, 0u);
    EXPECT_EQ(hdr->enrichment_source, 0u);
    EXPECT_EQ(hdr->compression_algo, 0u);
}
