#include "cortrix/spc/recursive_chunker.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace cortrix {

RecursiveChunker::RecursiveChunker(const ChunkConfig& config)
    : config_(config) {}

// Approximate token count:
// - CJK chars: ~2 tokens per char
// - ASCII words: ~1.3 tokens per word (approx 1 token per 4 chars)
static int ApproxTokenCount(const std::string& text) {
    if (text.empty()) return 0;

    int tokens = 0;
    size_t i = 0;
    while (i < text.size()) {
        uint8_t c = static_cast<uint8_t>(text[i]);
        if (c < 0x80) {
            // ASCII
            if (c == ' ' || c == '\n' || c == '\t' || c == '\r') {
                ++i;
                continue;
            }
            // Count ASCII word
            size_t word_start = i;
            while (i < text.size() && static_cast<uint8_t>(text[i]) < 0x80 &&
                   text[i] != ' ' && text[i] != '\n' && text[i] != '\t' && text[i] != '\r') {
                ++i;
            }
            // ~1.3 tokens per word, approximate as (len + 2) / 3
            size_t word_len = i - word_start;
            tokens += std::max(1, static_cast<int>((word_len + 2) / 3));
        } else {
            // Multi-byte UTF-8 char
            // CJK ranges: most multi-byte chars treated as ~2 tokens
            if (c >= 0xE0) {
                tokens += 2;
            } else {
                tokens += 1;
            }
            // Skip remaining bytes of multi-byte sequence
            if (c >= 0xF0) i += 4;
            else if (c >= 0xE0) i += 3;
            else i += 2;
            if (i > text.size()) i = text.size();
        }
    }
    return std::max(1, tokens);
}

// Find a UTF-8 safe boundary at or before position pos
static size_t Utf8SafeBoundary(const std::string& text, size_t pos) {
    if (pos >= text.size()) return text.size();
    // If we're at the start of a UTF-8 sequence or ASCII, we're fine
    // If we're in the middle of a multi-byte sequence, back up
    while (pos > 0 && (static_cast<uint8_t>(text[pos]) & 0xC0) == 0x80) {
        --pos;
    }
    return pos;
}

// Split text by a separator, returning the pieces
static std::vector<std::string> SplitBySeparator(const std::string& text,
                                                   const std::string& sep) {
    std::vector<std::string> pieces;
    if (sep.empty()) {
        // Character-level split (UTF-8 aware)
        size_t i = 0;
        while (i < text.size()) {
            uint8_t c = static_cast<uint8_t>(text[i]);
            size_t char_len = 1;
            if (c >= 0xF0) char_len = 4;
            else if (c >= 0xE0) char_len = 3;
            else if (c >= 0xC0) char_len = 2;
            if (i + char_len > text.size()) char_len = text.size() - i;
            pieces.push_back(text.substr(i, char_len));
            i += char_len;
        }
        return pieces;
    }

    size_t start = 0;
    size_t pos = text.find(sep, start);
    while (pos != std::string::npos) {
        pieces.push_back(text.substr(start, pos - start));
        start = pos + sep.size();
        pos = text.find(sep, start);
    }
    pieces.push_back(text.substr(start));
    return pieces;
}

// Merge small pieces together respecting chunk_size
static std::vector<std::string> MergePieces(const std::vector<std::string>& pieces,
                                              const std::string& sep,
                                              int chunk_size) {
    std::vector<std::string> merged;
    std::string current;

    for (const auto& piece : pieces) {
        if (piece.empty()) continue;

        std::string candidate;
        if (current.empty()) {
            candidate = piece;
        } else {
            candidate = current + sep + piece;
        }

        if (ApproxTokenCount(candidate) <= chunk_size) {
            current = candidate;
        } else {
            if (!current.empty()) {
                merged.push_back(current);
            }
            current = piece;
        }
    }
    if (!current.empty()) {
        merged.push_back(current);
    }
    return merged;
}

std::vector<ChunkResult> RecursiveChunker::SplitRecursive(const std::string& text, int depth) {
    if (text.empty()) return {};

    int token_count = ApproxTokenCount(text);
    if (token_count <= config_.chunk_size) {
        ChunkResult cr;
        cr.text = text;
        cr.token_count_approx = token_count;
        return {cr};
    }

    // Find separator to use at this depth
    const auto& seps = config_.separators;
    if (depth >= static_cast<int>(seps.size())) {
        // Hard cut at chunk_size boundary (UTF-8 safe)
        std::vector<ChunkResult> results;
        size_t pos = 0;
        while (pos < text.size()) {
            // Find approximate byte position for chunk_size tokens
            size_t end = std::min(pos + static_cast<size_t>(config_.chunk_size) * 3, text.size());
            end = Utf8SafeBoundary(text, end);
            if (end <= pos) end = pos + 1;

            std::string chunk = text.substr(pos, end - pos);
            while (ApproxTokenCount(chunk) > config_.chunk_size && chunk.size() > 1) {
                end = Utf8SafeBoundary(text, pos + (end - pos) * 3 / 4);
                if (end <= pos) { end = pos + 1; break; }
                chunk = text.substr(pos, end - pos);
            }

            ChunkResult cr;
            cr.text = chunk;
            cr.token_count_approx = ApproxTokenCount(chunk);
            results.push_back(cr);
            pos = end;
        }
        return results;
    }

    const std::string& sep = seps[depth];
    auto pieces = SplitBySeparator(text, sep);

    // Merge small pieces
    auto merged = MergePieces(pieces, sep, config_.chunk_size);

    // Recursively split any pieces that are still too large
    std::vector<ChunkResult> results;
    for (const auto& piece : merged) {
        if (ApproxTokenCount(piece) > config_.chunk_size) {
            auto sub_results = SplitRecursive(piece, depth + 1);
            for (auto& sr : sub_results) {
                results.push_back(std::move(sr));
            }
        } else {
            ChunkResult cr;
            cr.text = piece;
            cr.token_count_approx = ApproxTokenCount(piece);
            results.push_back(cr);
        }
    }

    return results;
}

std::vector<ChunkResult> RecursiveChunker::Chunk(const std::string& text) {
    if (text.empty()) return {};

    auto raw_chunks = SplitRecursive(text, 0);

    // Now apply overlap and fill in offsets
    std::vector<ChunkResult> results;
    int chunk_idx = 0;

    // Find positions of each chunk in original text for offset tracking
    size_t search_from = 0;
    for (auto& chunk : raw_chunks) {
        size_t pos = text.find(chunk.text, search_from);
        if (pos == std::string::npos) {
            pos = search_from;
        }
        chunk.chunk_index = chunk_idx++;
        chunk.start_offset = static_cast<int>(pos);
        chunk.end_offset = static_cast<int>(pos + chunk.text.size());
        search_from = pos + 1;
    }

    // Apply overlap: prepend overlap_chars from previous chunk
    if (config_.chunk_overlap > 0 && raw_chunks.size() > 1) {
        for (size_t i = 1; i < raw_chunks.size(); ++i) {
            const auto& prev = raw_chunks[i - 1];
            int overlap_bytes = 0;
            int overlap_tokens = 0;

            // Walk backward from end of prev chunk to get ~overlap tokens
            int pos = static_cast<int>(prev.text.size());
            while (pos > 0 && overlap_tokens < config_.chunk_overlap) {
                --pos;
                pos = static_cast<int>(Utf8SafeBoundary(prev.text, pos));
                overlap_tokens = ApproxTokenCount(prev.text.substr(pos));
            }
            overlap_bytes = static_cast<int>(prev.text.size()) - pos;

            if (overlap_bytes > 0 && overlap_bytes < static_cast<int>(prev.text.size())) {
                std::string overlap_text = prev.text.substr(prev.text.size() - overlap_bytes);
                raw_chunks[i].text = overlap_text + raw_chunks[i].text;
                raw_chunks[i].start_offset -= overlap_bytes;
                if (raw_chunks[i].start_offset < 0) raw_chunks[i].start_offset = 0;
                raw_chunks[i].token_count_approx = ApproxTokenCount(raw_chunks[i].text);
            }
        }
    }

    return raw_chunks;
}

}  // namespace cortrix
