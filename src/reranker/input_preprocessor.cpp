#include "cortrix/reranker/input_preprocessor.h"

#include <algorithm>
#include <cstdint>

#include <spdlog/spdlog.h>

#include "cortrix/reranker/reranker_metrics.h"

namespace cortrix::reranker {

namespace {

// UTF-8-safe boundary at or before `pos` (mirrors SPC recursive_chunker).
size_t Utf8SafeBoundary(const std::string& text, size_t pos) {
    if (pos >= text.size()) return text.size();
    while (pos > 0 && (static_cast<uint8_t>(text[pos]) & 0xC0) == 0x80) --pos;
    return pos;
}

}  // namespace

int InputPreprocessor::ApproxTokenCount(const std::string& text) {
    // Identical heuristic to SPC recursive_chunker's ApproxTokenCount so the
    // reranker's length judgment aligns with how chunks were sized upstream:
    // ASCII words ≈ (len+2)/3 tokens (~1.3 tok/word), CJK/3-byte chars ≈ 2 tokens.
    if (text.empty()) return 0;
    int tokens = 0;
    size_t i = 0;
    while (i < text.size()) {
        uint8_t c = static_cast<uint8_t>(text[i]);
        if (c < 0x80) {
            if (c == ' ' || c == '\n' || c == '\t' || c == '\r') {
                ++i;
                continue;
            }
            size_t word_start = i;
            while (i < text.size() && static_cast<uint8_t>(text[i]) < 0x80 &&
                   text[i] != ' ' && text[i] != '\n' && text[i] != '\t' && text[i] != '\r') {
                ++i;
            }
            size_t word_len = i - word_start;
            tokens += std::max(1, static_cast<int>((word_len + 2) / 3));
        } else {
            tokens += (c >= 0xE0) ? 2 : 1;
            if (c >= 0xF0) i += 4;
            else if (c >= 0xE0) i += 3;
            else i += 2;
            if (i > text.size()) i = text.size();
        }
    }
    return std::max(1, tokens);
}

int PassageTokenBudget(int query_tokens, int max_seq_length) {
    int budget = max_seq_length - query_tokens - kReservedSpecialTokens;
    return std::max(0, budget);
}

InputCategory CategorizeInput(int query_tokens, int passage_tokens, int max_seq_length) {
    if (passage_tokens <= PassageTokenBudget(query_tokens, max_seq_length)) {
        return InputCategory::kNormal;
    }
    if (passage_tokens > 4 * max_seq_length) {
        return InputCategory::kExtremelyLong;
    }
    return InputCategory::kTruncated;
}

std::string InputPreprocessor::TruncateToTokens(const std::string& text, int max_tokens) {
    if (max_tokens <= 0) return std::string();
    // Walk the same tokenization heuristic, stopping once `max_tokens` is reached,
    // then cut at a UTF-8-safe boundary. Preserves the front of the passage.
    int tokens = 0;
    size_t i = 0;
    while (i < text.size()) {
        uint8_t c = static_cast<uint8_t>(text[i]);
        size_t seg_start = i;
        int seg_tokens;
        if (c < 0x80) {
            if (c == ' ' || c == '\n' || c == '\t' || c == '\r') {
                ++i;
                continue;  // whitespace contributes 0 tokens
            }
            while (i < text.size() && static_cast<uint8_t>(text[i]) < 0x80 &&
                   text[i] != ' ' && text[i] != '\n' && text[i] != '\t' && text[i] != '\r') {
                ++i;
            }
            seg_tokens = std::max(1, static_cast<int>((i - seg_start + 2) / 3));
        } else {
            seg_tokens = (c >= 0xE0) ? 2 : 1;
            if (c >= 0xF0) i += 4;
            else if (c >= 0xE0) i += 3;
            else i += 2;
            if (i > text.size()) i = text.size();
        }
        if (tokens + seg_tokens > max_tokens) {
            // This segment would overflow the budget → cut before it.
            return text.substr(0, Utf8SafeBoundary(text, seg_start));
        }
        tokens += seg_tokens;
    }
    return text;  // already within budget
}

PreprocessedPassage InputPreprocessor::Prepare(const std::string& query,
                                               const std::string& passage,
                                               int max_seq_length) {
    const int query_tokens = ApproxTokenCount(query);
    const int passage_tokens = ApproxTokenCount(passage);
    const InputCategory cat = CategorizeInput(query_tokens, passage_tokens, max_seq_length);

    switch (cat) {
        case InputCategory::kNormal:
            return {InputCategory::kNormal, passage, false};

        case InputCategory::kTruncated: {
            const int budget = PassageTokenBudget(query_tokens, max_seq_length);
            std::string truncated = TruncateToTokens(passage, budget);
            spdlog::warn(
                "Reranker: passage truncated ({} -> ~{} tokens, max_seq_length={}, "
                "query={} tokens); relevance may degrade",
                passage_tokens, budget, max_seq_length, query_tokens);
            RerankerMetrics::Instance().RecordTruncated();
            return {InputCategory::kTruncated, std::move(truncated), false};
        }

        case InputCategory::kExtremelyLong:
            //: data-quality problem — do NOT raise a PG ERROR to the query
            // user (no "re-upload" entry point); surface via LOG_ERROR + metric so
            // ops sees it. The candidate gets score=0 (ranked last).
            spdlog::error(
                "Reranker: extremely long passage ({} tokens > 4×max_seq_length={}); "
                "scoring as 0 (likely SPC bypass / data corruption)",
                passage_tokens, 4 * max_seq_length);
            RerankerMetrics::Instance().RecordExtremelyLong();
            return {InputCategory::kExtremelyLong, std::string(), true};
    }
    // Unreachable for a valid enum; default to NORMAL passthrough.
    return {InputCategory::kNormal, passage, false};
}

}  // namespace cortrix::reranker
