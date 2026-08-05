#include <gtest/gtest.h>
#include "cortrix/spc/block_assembler.h"
#include "cortrix/common/block_header.h"
#include "cortrix/common/block_types.h"
#include "cortrix/id/hash.h"   // SetDeploymentHashKeyForTesting / HashChildIdToBlockId

namespace cortrix {
namespace {

TEST(BlockAssemblerTest, AssembleBasicBlock) {
    BlockAssembler assembler;

    ChunkResult chunk;
    chunk.text = "Hello world, this is a test chunk.";
    chunk.chunk_index = 0;
    chunk.start_offset = 0;
    chunk.end_offset = 33;
    chunk.token_count_approx = 8;

    EmbeddingResult embedding;
    embedding.dim = 4;
    embedding.vector = {0.1f, 0.2f, 0.3f, 0.4f};

    CortrixBlock block = assembler.Assemble(
        "01JTESTDOC0000000000000042", chunk, embedding, kBlockFile, 3, "{\"source\":\"test\"}");

    EXPECT_EQ(block.doc_id, "01JTESTDOC0000000000000042");
    EXPECT_EQ(block.chunk_index, 0);
    EXPECT_EQ(block.block_type, static_cast<int>(kBlockFile));
    EXPECT_EQ(block.processing_level, static_cast<int>(kLevelL3));
    EXPECT_EQ(block.content_text, "Hello world, this is a test chunk.");

    // Verify binary data has valid block header
    ASSERT_GE(block.data.size(), 128u);  // At least header size
    const cortrix_block_header_t* hdr = nullptr;
    bool valid = BlockParse(block.data.data(), block.data.size(), &hdr);
    ASSERT_TRUE(valid);
    EXPECT_EQ(hdr->magic, kBlockMagic);
    EXPECT_EQ(hdr->header_version, kHeaderVersion);
    EXPECT_EQ(hdr->header_size, kHeaderSize);
    EXPECT_EQ(hdr->block_type, static_cast<uint16_t>(kBlockFile));
}

TEST(BlockAssemblerTest, AssembleWithMetadata) {
    BlockAssembler assembler;

    ChunkResult chunk;
    chunk.text = "Test content";
    chunk.chunk_index = 5;

    EmbeddingResult embedding;
    embedding.dim = 1024;
    embedding.vector.resize(1024, 0.1f);

    std::string metadata = "{\"page\":1,\"section\":\"intro\"}";
    CortrixBlock block = assembler.Assemble(
        "01JTESTDOC0000000000000100", chunk, embedding, kBlockScan, 3, metadata);

    EXPECT_EQ(block.doc_id, "01JTESTDOC0000000000000100");
    EXPECT_EQ(block.chunk_index, 5);
    EXPECT_EQ(block.block_type, static_cast<int>(kBlockScan));
}

TEST(BlockAssemblerTest, AssembleEmptyMetadata) {
    BlockAssembler assembler;

    ChunkResult chunk;
    chunk.text = "Simple text";
    chunk.chunk_index = 0;

    EmbeddingResult embedding;
    embedding.dim = 4;
    embedding.vector = {1.0f, 0.0f, 0.0f, 0.0f};

    CortrixBlock block = assembler.Assemble("01JTESTDOC0000000000000001", chunk, embedding, kBlockFile);
    EXPECT_EQ(block.doc_id, "01JTESTDOC0000000000000001");
    EXPECT_FALSE(block.data.empty());
}

TEST(BlockAssemblerTest, MintsNonZeroUniqueBlockId) {
    // D3.5 wire⑤: every assembled block gets a real uint64 block_id =
    // HashChildIdToBlockId(a freshly-minted child_id ULID), not a DB rowid. Two
    // assembles of the same chunk get distinct ids (distinct child_ids).
    id::SetDeploymentHashKeyForTesting({0x0123456789abcdefULL, 0xfedcba9876543210ULL});
    BlockAssembler assembler;
    ChunkResult chunk;
    chunk.text = "same text";
    chunk.chunk_index = 0;
    EmbeddingResult embedding;
    embedding.dim = 4;
    embedding.vector = {0.1f, 0.2f, 0.3f, 0.4f};

    CortrixBlock b1 = assembler.Assemble("01JTESTDOC0000000000000001", chunk, embedding, kBlockFile, 3, "");
    CortrixBlock b2 = assembler.Assemble("01JTESTDOC0000000000000001", chunk, embedding, kBlockFile, 3, "");
    EXPECT_NE(b1.block_id, 0u);
    EXPECT_NE(b2.block_id, 0u);
    EXPECT_NE(b1.block_id, b2.block_id) << "distinct child_ids → distinct block_ids";
}

// unified-blocks: AssembleChild: a child block carries the real child_id (so
// block_id = HashChildIdToBlockId(child_id)), block_type = source modality (candidate
// B, not kBlockChild), and the child columns.
TEST(BlockAssemblerTest, AssembleChildCarriesAColumns) {
    id::SetDeploymentHashKeyForTesting({0x0123456789abcdefULL, 0xfedcba9876543210ULL});
    BlockAssembler assembler;

    cortrix::chunker::ChildChunk child;
    child.child_id = "01HXCHILD0000000000000007";
    child.parent_id = "01HXPARENT000000000000007";
    child.doc_id = "01JTESTDOC0000000000000007";
    child.child_text = "a focused 200-token child unit";
    child.token_count = 55;
    child.parent_offset = 12;
    child.chunk_index = 3;

    EmbeddingResult embedding;
    embedding.dim = 4;
    embedding.vector = {0.1f, 0.2f, 0.3f, 0.4f};

    CortrixBlock b = assembler.AssembleChild(child, embedding, kBlockFile, 3,
                                             R"({"ner":["Acme"]})");

    // block_id is the hash of the *real* child_id (the P-HNSW label).
    EXPECT_EQ(b.block_id, id::HashChildIdToBlockId("01HXCHILD0000000000000007"));
    EXPECT_EQ(b.block_type, static_cast<int>(kBlockFile));  // candidate B: source modality
    EXPECT_EQ(b.doc_id, "01JTESTDOC0000000000000007");
    EXPECT_EQ(b.chunk_index, 3);
    EXPECT_EQ(b.content_text, "a focused 200-token child unit");
    EXPECT_EQ(b.child_id, "01HXCHILD0000000000000007");
    EXPECT_EQ(b.parent_id, "01HXPARENT000000000000007");
    EXPECT_EQ(b.token_count, 55);
    EXPECT_EQ(b.parent_offset, 12);
    EXPECT_EQ(b.metadata_json, R"({"ner":["Acme"]})");
    ASSERT_GE(b.data.size(), 128u);  // valid binary block payload too
}

}  // namespace
}  // namespace cortrix
