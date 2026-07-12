#include <gtest/gtest.h>
#include "cortrix/spc/recursive_chunker.h"
#include "cortrix/spc/onnx_embedder.h"
#include "cortrix/spc/block_assembler.h"
#include "cortrix/spc/spc_router.h"
#include "cortrix/spc/spc_pipeline.h"
#include "cortrix/spc/spc_task.h"
#include "cortrix/spc/spc_task_queue.h"
#include "cortrix/common/block_header.h"
#include "cortrix/common/block_types.h"
#include "cortrix/config/config.h"
#include <fstream>
#include <sstream>
#include <cstdio>

namespace cortrix {
namespace {

// Read a text file into a string. In-process equivalent of the (removed in R6b)
// MVP TxtParser/MarkdownParser used purely to obtain text for the chunker; the
// F06 parser path (DoclingParser/PaddleOCRParser) runs via a Python subprocess
// bridge. These tests exercise the chunk -> embed -> assemble chain, not parsing.
static std::string ReadFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// ---- Helper: create temp files for testing ----

class SPCEndToEndTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Unique per test: parallel ctest processes must not share these files
        // (a sibling's TearDown/remove would yank them mid-parse).
        const std::string uniq =
            ::testing::UnitTest::GetInstance()->current_test_info()->name();
        txt_path_ = "/tmp/cortrix_e2e_test_" + uniq + ".txt";
        md_path_ = "/tmp/cortrix_e2e_test_" + uniq + ".md";

        {
            std::ofstream f(txt_path_);
            // Generate enough text to produce multiple chunks
            for (int i = 0; i < 100; ++i) {
                f << "This is paragraph " << i << " of the test document. "
                  << "It contains enough text to ensure the chunker produces "
                  << "multiple chunks when processing this file.\n\n";
            }
        }
        {
            std::ofstream f(md_path_);
            f << "# Test Document\n\n"
              << "## Section One\n\n"
              << "This is **bold** text in the first section. "
              << "It has [links](http://example.com) and `code`.\n\n";
            for (int i = 0; i < 50; ++i) {
                f << "Paragraph " << i << " contains some content for testing. "
                  << "The markdown parser should strip formatting markers.\n\n";
            }
        }
    }

    void TearDown() override {
        std::remove(txt_path_.c_str());
        std::remove(md_path_.c_str());
    }

    std::string txt_path_;
    std::string md_path_;
};

// ---- Test: Router correctly infers MIME types ----

TEST_F(SPCEndToEndTest, RouterInfersMimeTypes) {
    EXPECT_EQ(SPCRouter::InferMimeType("file.pdf"), "application/pdf");
    EXPECT_EQ(SPCRouter::InferMimeType("file.txt"), "text/plain");
    EXPECT_EQ(SPCRouter::InferMimeType("file.md"), "text/markdown");
    EXPECT_EQ(SPCRouter::InferMimeType("file.docx"),
              "application/vnd.openxmlformats-officedocument.wordprocessingml.document");
    EXPECT_EQ(SPCRouter::InferMimeType("file.png"), "image/png");
    EXPECT_EQ(SPCRouter::InferMimeType("file.jpg"), "image/jpeg");

    EXPECT_TRUE(SPCRouter::IsSupported("application/pdf"));
    EXPECT_TRUE(SPCRouter::IsSupported("text/plain"));
    EXPECT_FALSE(SPCRouter::IsSupported("application/octet-stream"));
}

// ---- Test: Full pipeline TXT: parse → chunk → embed → assemble ----

TEST_F(SPCEndToEndTest, FullPipelineTxt) {
    // 1. Read text (plain .txt input; see ReadFile note)
    std::string doc_text = ReadFile(txt_path_);
    ASSERT_FALSE(doc_text.empty());
    ASSERT_GT(doc_text.size(), 0u);

    // 2. Chunk
    ChunkConfig chunk_config;
    chunk_config.chunk_size = 512;
    chunk_config.chunk_overlap = 50;
    RecursiveChunker chunker(chunk_config);
    auto chunks = chunker.Chunk(doc_text);
    ASSERT_GT(chunks.size(), 1u) << "Expected multiple chunks from large text";

    // Verify chunk properties
    for (size_t i = 0; i < chunks.size(); ++i) {
        EXPECT_EQ(chunks[i].chunk_index, static_cast<int>(i));
        EXPECT_FALSE(chunks[i].text.empty());
        EXPECT_GT(chunks[i].token_count_approx, 0);
    }

    // 3. Embed (stub mode)
    OnnxEmbedder embedder("", 1024);
    Status s = embedder.Init();
    ASSERT_TRUE(s.ok()) << s.message();

    std::vector<std::string> chunk_texts;
    for (const auto& c : chunks) chunk_texts.push_back(c.text);

    std::vector<EmbeddingResult> embeddings;
    s = embedder.EmbedBatch(chunk_texts, &embeddings);
    ASSERT_TRUE(s.ok()) << s.message();
    ASSERT_EQ(embeddings.size(), chunks.size());

    // Verify embedding properties
    for (const auto& emb : embeddings) {
        EXPECT_EQ(emb.dim, 1024);
        EXPECT_EQ(static_cast<int>(emb.vector.size()), 1024);
        EXPECT_GT(emb.inference_time_ms, 0.0f);
    }

    // 4. Assemble
    BlockAssembler assembler;
    // [A unified-blocks / D-I6] doc_id is a ULID string (was int64_t before the id-system
    // migration); BlockAssembler::Assemble now takes const std::string&.
    std::string doc_id = "01JTESTDOC0000000000000042";
    for (size_t i = 0; i < chunks.size(); ++i) {
        CortrixBlock block = assembler.Assemble(
            doc_id, chunks[i], embeddings[i], kBlockFile);

        EXPECT_EQ(block.doc_id, doc_id);
        EXPECT_EQ(block.chunk_index, chunks[i].chunk_index);
        EXPECT_EQ(block.block_type, static_cast<int>(kBlockFile));
        EXPECT_EQ(block.processing_level, static_cast<int>(kLevelL3));
        EXPECT_EQ(block.content_text, chunks[i].text);

        // Verify binary block data
        ASSERT_GE(block.data.size(), 128u);
        const cortrix_block_header_t* hdr = nullptr;
        bool valid = BlockParse(block.data.data(), block.data.size(), &hdr);
        ASSERT_TRUE(valid) << "Block header validation failed for chunk " << i;
        EXPECT_EQ(hdr->magic, kBlockMagic);
        EXPECT_EQ(hdr->block_type, static_cast<uint16_t>(kBlockFile));
        EXPECT_EQ(hdr->vector_dim, 1024);
    }
}

// ---- Test: Full pipeline Markdown: parse → chunk → embed → assemble ----

TEST_F(SPCEndToEndTest, FullPipelineMarkdown) {
    // 1. Read text (markdown input read as raw text; see ReadFile note).
    //    NOTE: the old MVP MarkdownParser stripped markdown markers ("**"), which
    //    was a parser-layer behavior. With the parser removed (R6b) the chunker
    //    receives raw markdown; this test now exercises chunk -> embed -> assemble
    //    only, so the marker-stripping assertion is intentionally dropped.
    std::string doc_text = ReadFile(md_path_);
    ASSERT_FALSE(doc_text.empty());

    // 2. Chunk
    RecursiveChunker chunker;
    auto chunks = chunker.Chunk(doc_text);
    ASSERT_GE(chunks.size(), 1u);

    // 3. Embed
    OnnxEmbedder embedder("", 1024);
    ASSERT_TRUE(embedder.Init().ok());

    std::vector<std::string> texts;
    for (const auto& c : chunks) texts.push_back(c.text);

    std::vector<EmbeddingResult> embeddings;
    ASSERT_TRUE(embedder.EmbedBatch(texts, &embeddings).ok());
    ASSERT_EQ(embeddings.size(), chunks.size());

    // 4. Assemble
    BlockAssembler assembler;
    for (size_t i = 0; i < chunks.size(); ++i) {
        CortrixBlock block = assembler.Assemble(
            "01JTESTDOC0000000000000099", chunks[i], embeddings[i], kBlockFile);
        EXPECT_EQ(block.doc_id, "01JTESTDOC0000000000000099");
        EXPECT_FALSE(block.data.empty());
    }
}

// ---- Test: Embedder produces different vectors for different texts ----

TEST_F(SPCEndToEndTest, EmbedderDeterministicButVaried) {
    OnnxEmbedder embedder("", 1024);
    ASSERT_TRUE(embedder.Init().ok());

    EmbeddingResult r1, r2, r3;
    ASSERT_TRUE(embedder.Embed("hello world", &r1).ok());
    ASSERT_TRUE(embedder.Embed("goodbye world", &r2).ok());
    ASSERT_TRUE(embedder.Embed("hello world", &r3).ok());

    // Same text → same vector (deterministic)
    EXPECT_EQ(r1.vector, r3.vector);

    // Different text → different vector
    EXPECT_NE(r1.vector, r2.vector);

    // Vectors should be unit-normalized
    float norm = 0.0f;
    for (float v : r1.vector) norm += v * v;
    EXPECT_NEAR(norm, 1.0f, 0.01f);
}

// ---- Test: TaskQueue priority + cancel integration ----

TEST_F(SPCEndToEndTest, TaskQueueIntegration) {
    SPCTaskQueue queue(1000);

    // Submit mixed priority tasks
    for (int i = 0; i < 10; ++i) {
        auto task = std::make_shared<SPCTask>();
        task->priority = (i % 3 == 0) ? SPCPriority::kP0 :
                         (i % 3 == 1) ? SPCPriority::kP1 : SPCPriority::kP2;
        task->created_at_us = i * 1000;
        task->source_path = "file_" + std::to_string(i) + ".txt";
        EXPECT_EQ(queue.Push(task), 0);
    }
    EXPECT_EQ(queue.Size(), 10u);

    // Cancel some tasks
    int cancelled = queue.CancelBySourcePath("file_3.txt");
    EXPECT_EQ(cancelled, 1);

    // Drain queue — P0 tasks should come first
    SPCPriority last_pri = SPCPriority::kP0;
    int popped = 0;
    while (queue.Size() > 0) {
        auto task = queue.Pop();
        if (!task || task->cancelled.load()) continue;
        EXPECT_LE(static_cast<int>(last_pri), static_cast<int>(task->priority));
        last_pri = task->priority;
        ++popped;
    }
    EXPECT_GT(popped, 0);

    queue.Stop();
}

// ---- Test: Block header CRC32 integrity ----

TEST_F(SPCEndToEndTest, BlockHeaderCrc32Integrity) {
    // Build a block and verify CRC
    auto data = BlockBuild(kBlockFile, kLevelL3, "test content",
                           "{\"key\":\"value\"}", "parser->chunker->embedder",
                           1024, 1);

    ASSERT_GE(data.size(), 128u);

    const cortrix_block_header_t* hdr = nullptr;
    bool valid = BlockParse(data.data(), data.size(), &hdr);
    ASSERT_TRUE(valid);

    // Verify fields
    EXPECT_EQ(hdr->magic, kBlockMagic);
    EXPECT_EQ(hdr->header_version, kHeaderVersion);
    EXPECT_EQ(hdr->header_size, kHeaderSize);
    EXPECT_EQ(hdr->block_type, static_cast<uint16_t>(kBlockFile));
    EXPECT_EQ(hdr->processing_level, static_cast<uint8_t>(kLevelL3));
    EXPECT_EQ(hdr->vector_dim, 1024);
    EXPECT_GT(hdr->content_length, 0u);
    EXPECT_EQ(hdr->total_size, static_cast<uint32_t>(data.size()));

    // Verify CRC matches
    uint32_t computed_crc = BlockComputeCrc32(data.data(), data.size());
    EXPECT_EQ(hdr->crc32, computed_crc);
}

}  // namespace
}  // namespace cortrix
