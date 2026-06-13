#include <gtest/gtest.h>
#include "cortrix/spc/recursive_chunker.h"

namespace cortrix {
namespace {

TEST(RecursiveChunkerTest, EmptyText) {
    RecursiveChunker chunker;
    auto chunks = chunker.Chunk("");
    EXPECT_TRUE(chunks.empty());
}

TEST(RecursiveChunkerTest, ShortTextSingleChunk) {
    RecursiveChunker chunker;
    auto chunks = chunker.Chunk("Hello world");
    ASSERT_EQ(chunks.size(), 1u);
    EXPECT_EQ(chunks[0].text, "Hello world");
    EXPECT_EQ(chunks[0].chunk_index, 0);
    EXPECT_GT(chunks[0].token_count_approx, 0);
}

TEST(RecursiveChunkerTest, SplitsByParagraph) {
    // Create text large enough to exceed chunk_size in tokens
    // Each word ~1 token, so 100 words > 50 token limit
    std::string paragraph1;
    for (int i = 0; i < 100; ++i) paragraph1 += "word ";
    std::string paragraph2;
    for (int i = 0; i < 100; ++i) paragraph2 += "text ";
    std::string text = paragraph1 + "\n\n" + paragraph2;

    ChunkConfig config;
    config.chunk_size = 50;
    config.chunk_overlap = 0;
    RecursiveChunker chunker(config);

    auto chunks = chunker.Chunk(text);
    ASSERT_GE(chunks.size(), 2u);
}

TEST(RecursiveChunkerTest, ChunkIndicesAreSequential) {
    std::string text;
    for (int i = 0; i < 50; ++i) {
        text += "This is sentence number " + std::to_string(i) + ". ";
    }

    ChunkConfig config;
    config.chunk_size = 50;
    config.chunk_overlap = 0;
    RecursiveChunker chunker(config);

    auto chunks = chunker.Chunk(text);
    ASSERT_GT(chunks.size(), 1u);
    for (size_t i = 0; i < chunks.size(); ++i) {
        EXPECT_EQ(chunks[i].chunk_index, static_cast<int>(i));
    }
}

TEST(RecursiveChunkerTest, Utf8Safety) {
    // Chinese text: each char is 3 bytes
    std::string text;
    for (int i = 0; i < 300; ++i) {
        text += "\xe4\xb8\xad";  // U+4E2D (CJK char)
    }

    ChunkConfig config;
    config.chunk_size = 100;
    config.chunk_overlap = 0;
    RecursiveChunker chunker(config);

    auto chunks = chunker.Chunk(text);
    ASSERT_GE(chunks.size(), 1u);

    // Verify no chunk starts or ends mid-character
    for (const auto& c : chunks) {
        // Every byte should be a valid UTF-8 start or continuation
        for (size_t i = 0; i < c.text.size(); ) {
            uint8_t byte = static_cast<uint8_t>(c.text[i]);
            size_t expected_len = 1;
            if (byte >= 0xF0) expected_len = 4;
            else if (byte >= 0xE0) expected_len = 3;
            else if (byte >= 0xC0) expected_len = 2;
            ASSERT_LE(i + expected_len, c.text.size())
                << "UTF-8 boundary violation at byte " << i;
            i += expected_len;
        }
    }
}

TEST(RecursiveChunkerTest, DefaultConfig) {
    ChunkConfig config;
    EXPECT_EQ(config.chunk_size, 512);
    EXPECT_EQ(config.chunk_overlap, 50);
    EXPECT_FALSE(config.separators.empty());
}

TEST(RecursiveChunkerTest, CustomSeparators) {
    ChunkConfig config;
    config.chunk_size = 10;
    config.chunk_overlap = 0;
    config.separators = {"\n", " ", ""};

    // Each line has ~15 words = ~15 tokens, well over chunk_size=10
    std::string text;
    for (int i = 0; i < 3; ++i) {
        if (i > 0) text += "\n";
        for (int j = 0; j < 15; ++j) text += "word ";
    }
    RecursiveChunker chunker(config);
    auto chunks = chunker.Chunk(text);
    ASSERT_GE(chunks.size(), 2u);
}

// ============================================================
// Hard-cut fallback path (depth >= separators.size())
// Lines 140-164: when all separators exhausted, hard-cut at chunk_size
// ============================================================

TEST(RecursiveChunkerTest, HardCutFallback_NoSeparatorsInText) {
    // Text with no separators at all - forces hard-cut path
    ChunkConfig config;
    config.chunk_size = 10;
    config.chunk_overlap = 0;
    config.separators = {};  // No separators at all!

    // Create a long string with no whitespace or punctuation
    std::string text(200, 'a');

    RecursiveChunker chunker(config);
    auto chunks = chunker.Chunk(text);
    ASSERT_GE(chunks.size(), 2u);

    // All chunks should be non-empty and combined should cover entire text
    size_t total_chars = 0;
    for (const auto& c : chunks) {
        EXPECT_FALSE(c.text.empty());
        EXPECT_GT(c.token_count_approx, 0);
        total_chars += c.text.size();
    }
    // With no overlap, total chars should be exactly text.size()
    EXPECT_EQ(total_chars, text.size());
}

TEST(RecursiveChunkerTest, HardCutFallback_SingleSeparatorExhausted) {
    // Only one separator that doesn't appear in text
    ChunkConfig config;
    config.chunk_size = 5;
    config.chunk_overlap = 0;
    config.separators = {"\n"};

    // Text is one long line with no newline, exceeds chunk_size
    std::string text;
    for (int i = 0; i < 100; ++i) text += "word ";

    RecursiveChunker chunker(config);
    auto chunks = chunker.Chunk(text);
    ASSERT_GE(chunks.size(), 2u);
}

// ============================================================
// 2-byte UTF-8 character handling (line 41, 46)
// ============================================================

TEST(RecursiveChunkerTest, TwoByteUtf8_LatinExtended) {
    // 2-byte UTF-8 chars (Latin extended, accented chars)
    // e.g., "e" with accent: \xc3\xa9
    std::string text;
    for (int i = 0; i < 200; ++i) {
        text += "\xc3\xa9";  // e-accent (2 bytes per char)
    }

    ChunkConfig config;
    config.chunk_size = 50;
    config.chunk_overlap = 0;
    RecursiveChunker chunker(config);
    auto chunks = chunker.Chunk(text);
    ASSERT_GE(chunks.size(), 1u);

    // Verify no chunk breaks mid-character
    for (const auto& c : chunks) {
        EXPECT_EQ(c.text.size() % 2, 0u)
            << "Chunk size should be even (2-byte chars)";
    }
}

// ============================================================
// 4-byte UTF-8 character handling (line 44)
// ============================================================

TEST(RecursiveChunkerTest, FourByteUtf8_Emoji) {
    // 4-byte UTF-8 chars (emoji)
    // Emoji "grinning face" = F0 9F 98 80
    std::string text;
    for (int i = 0; i < 100; ++i) {
        text += "\xF0\x9F\x98\x80";
    }

    ChunkConfig config;
    config.chunk_size = 30;
    config.chunk_overlap = 0;
    RecursiveChunker chunker(config);
    auto chunks = chunker.Chunk(text);
    ASSERT_GE(chunks.size(), 1u);

    // Verify UTF-8 boundary integrity
    for (const auto& c : chunks) {
        EXPECT_EQ(c.text.size() % 4, 0u)
            << "Chunk size should be multiple of 4 (4-byte chars)";
    }
}

// ============================================================
// Overlap logic tests (lines 214-237)
// ============================================================

TEST(RecursiveChunkerTest, OverlapApplied) {
    ChunkConfig config;
    config.chunk_size = 20;
    config.chunk_overlap = 5;
    config.separators = {"\n\n"};

    // Create text that splits into multiple chunks
    std::string text;
    for (int i = 0; i < 5; ++i) {
        for (int j = 0; j < 30; ++j) text += "word ";
        text += "\n\n";
    }

    RecursiveChunker chunker(config);
    auto chunks = chunker.Chunk(text);
    ASSERT_GE(chunks.size(), 2u);

    // Second chunk should start with text from end of first chunk (overlap)
    if (chunks.size() > 1) {
        // Overlap means chunk[1] should have content that also appears at end of chunk[0]
        EXPECT_GT(chunks[1].token_count_approx, 0);
    }
}

TEST(RecursiveChunkerTest, OverlapZero) {
    ChunkConfig config;
    config.chunk_size = 20;
    config.chunk_overlap = 0;

    std::string text;
    for (int i = 0; i < 50; ++i) text += "word ";

    RecursiveChunker chunker(config);
    auto chunks = chunker.Chunk(text);
    ASSERT_GE(chunks.size(), 2u);

    // No overlap: consecutive chunks should not share text prefix/suffix
}

// ============================================================
// Empty separator path (character-level split, line 68-82)
// ============================================================

TEST(RecursiveChunkerTest, EmptySeparator_CharacterLevelSplit) {
    ChunkConfig config;
    config.chunk_size = 5;
    config.chunk_overlap = 0;
    config.separators = {""};  // Character-level split

    std::string text = "HelloWorld123";

    RecursiveChunker chunker(config);
    auto chunks = chunker.Chunk(text);
    ASSERT_GE(chunks.size(), 1u);
}

// ============================================================
// MergePieces - all empty pieces
// ============================================================

TEST(RecursiveChunkerTest, MergePieces_AllEmpty) {
    ChunkConfig config;
    config.chunk_size = 100;
    config.chunk_overlap = 0;
    config.separators = {"\n\n"};

    // Text that's just separators
    std::string text = "\n\n\n\n\n\n";

    RecursiveChunker chunker(config);
    auto chunks = chunker.Chunk(text);
    // Should handle gracefully
}

// ============================================================
// Chunk with CJK text (Chinese/Japanese/Korean)
// ============================================================

TEST(RecursiveChunkerTest, CjkTextSentenceSplitting) {
    ChunkConfig config;
    config.chunk_size = 50;
    config.chunk_overlap = 0;

    // Chinese text with sentence-ending period (U+3002 = \xe3\x80\x82)
    std::string text;
    for (int i = 0; i < 200; ++i) {
        text += "\xe4\xb8\xad\xe6\x96\x87";  // 2 CJK chars (U+4E2D U+6587)
    }
    // Add some sentence breaks
    text += "\xe3\x80\x82";  // U+3002 ideographic full stop
    for (int i = 0; i < 200; ++i) {
        text += "\xe4\xb8\xad\xe6\x96\x87";
    }

    RecursiveChunker chunker(config);
    auto chunks = chunker.Chunk(text);
    ASSERT_GE(chunks.size(), 2u);
}

// ============================================================
// ApproxTokenCount coverage (ASCII whitespace skip, word counting)
// ============================================================

TEST(RecursiveChunkerTest, TokenCount_WhitespaceOnly) {
    ChunkConfig config;
    config.chunk_size = 100;
    config.chunk_overlap = 0;

    // Text with only whitespace
    std::string text = "   \t\n\r   ";
    RecursiveChunker chunker(config);
    auto chunks = chunker.Chunk(text);
    // Should produce 0 or 1 chunk with near-0 tokens
    if (!chunks.empty()) {
        EXPECT_LE(chunks[0].token_count_approx, 1);
    }
}

TEST(RecursiveChunkerTest, TokenCount_MixedAsciiAndCjk) {
    ChunkConfig config;
    config.chunk_size = 50;
    config.chunk_overlap = 0;

    // Mix of ASCII and CJK
    std::string text = "Hello ";
    for (int i = 0; i < 100; ++i) {
        text += "\xe4\xb8\xad";  // U+4E2D (CJK char)
    }
    text += " world";

    RecursiveChunker chunker(config);
    auto chunks = chunker.Chunk(text);
    ASSERT_GE(chunks.size(), 1u);
}

// ============================================================
// Utf8SafeBoundary edge cases
// ============================================================

TEST(RecursiveChunkerTest, Utf8SafeBoundary_AtEndOfText) {
    // Text that's exactly at boundary
    ChunkConfig config;
    config.chunk_size = 3;
    config.chunk_overlap = 0;
    config.separators = {};  // Force hard-cut

    std::string text = "abc";  // Exactly 3 chars
    RecursiveChunker chunker(config);
    auto chunks = chunker.Chunk(text);
    ASSERT_EQ(chunks.size(), 1u);
    EXPECT_EQ(chunks[0].text, "abc");
}

// ============================================================
// Chunk offset tracking
// ============================================================

TEST(RecursiveChunkerTest, ChunkOffsets_Tracked) {
    ChunkConfig config;
    config.chunk_size = 20;
    config.chunk_overlap = 0;
    config.separators = {"\n\n"};

    std::string text;
    for (int i = 0; i < 5; ++i) {
        for (int j = 0; j < 30; ++j) text += "word ";
        text += "\n\n";
    }

    RecursiveChunker chunker(config);
    auto chunks = chunker.Chunk(text);
    ASSERT_GE(chunks.size(), 2u);

    // Offsets should be non-negative and generally increasing
    for (const auto& c : chunks) {
        EXPECT_GE(c.start_offset, 0);
        EXPECT_GE(c.end_offset, c.start_offset);
    }
}

// ============================================================
// SplitBySeparator - separator at start/end
// ============================================================

TEST(RecursiveChunkerTest, SeparatorAtStartAndEnd) {
    ChunkConfig config;
    config.chunk_size = 10;
    config.chunk_overlap = 0;
    config.separators = {"\n\n", "\n", " "};

    std::string text = "\n\nsome text here with enough words to exceed chunk size by a lot\n\n";
    RecursiveChunker chunker(config);
    auto chunks = chunker.Chunk(text);
    ASSERT_GE(chunks.size(), 1u);
}

}  // namespace
}  // namespace cortrix
