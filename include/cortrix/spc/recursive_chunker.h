#pragma once
#include <string>
#include <vector>

namespace cortrix {

struct ChunkResult {
    std::string text;
    int chunk_index = 0;
    int start_offset = 0;
    int end_offset = 0;
    int token_count_approx = 0;
};

struct ChunkConfig {
    int chunk_size = 512;
    int chunk_overlap = 50;
    std::vector<std::string> separators = {
        "\f",
        "\n\n", "\n",
        "\xe3\x80\x82", ".",
        "\xef\xbc\x81", "\xef\xbc\x9f",
        "!", "?",
        "\xef\xbc\x9b", ";",
        " ", ""
    };
};

/// Recursive text splitter with configurable separators
class RecursiveChunker {
public:
    explicit RecursiveChunker(const ChunkConfig& config = {});

    /// Split text into chunks
    std::vector<ChunkResult> Chunk(const std::string& text);

private:
    std::vector<ChunkResult> SplitRecursive(const std::string& text, int depth);
    ChunkConfig config_;
};

}  // namespace cortrix
