#include <cstdint>
#include "cortrix/chunker/parent_child_chunker.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <utility>

#include "cortrix/chunker/chunker_errors.h"
#include "cortrix/id/ulid.h"
#include "cortrix/spc/recursive_chunker.h"  // RecursiveChunker — reused for child splitting

namespace cortrix::chunker {
namespace {

int64_t NowMs() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

// Trim ASCII whitespace from both ends; used to drop blank paragraphs (§ 4.5).
std::string Trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n\f");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n\f");
    return s.substr(b, e - b + 1);
}

}  // namespace

uint32_t EstimateTokens(const std::string& text) {
    // Char-based heuristic (standalone): ASCII run ≈ len/4 tokens (min 1 per run),
    // each non-ASCII UTF-8 char ≈ 1 token (CJK ≈ 1 token/char). Deterministic;
    // never 0 for non-empty text. Production wires the real bge-m3 tokenizer (D3.5).
    uint32_t tokens = 0;
    size_t i = 0;
    const size_t n = text.size();
    while (i < n) {
        uint8_t c = static_cast<uint8_t>(text[i]);
        if (c < 0x80) {
            if (c == ' ' || c == '\n' || c == '\t' || c == '\r' || c == '\f') { ++i; continue; }
            size_t start = i;
            while (i < n) {
                uint8_t d = static_cast<uint8_t>(text[i]);
                if (d >= 0x80 || d == ' ' || d == '\n' || d == '\t' || d == '\r' || d == '\f') break;
                ++i;
            }
            size_t len = i - start;
            tokens += std::max<uint32_t>(1, static_cast<uint32_t>((len + 3) / 4));
        } else {
            tokens += 1;
            size_t adv = (c >= 0xF0) ? 4 : (c >= 0xE0) ? 3 : (c >= 0xC0) ? 2 : 1;
            i += adv;
            if (i > n) i = n;
        }
    }
    // Whitespace-only text counts as 0 tokens; any real content is at least 1.
    return tokens;
}

ParentChildChunker::ParentChildChunker(ChunkerConfig config, TokenCounter token_counter)
    : config_(std::move(config)),
      token_counter_(token_counter ? std::move(token_counter) : EstimateTokens) {}

std::vector<ParentChildChunker::Unit> ParentChildChunker::CollectUnits(
    const ChunkerInput& input, ChunkerStats* stats) const {
    std::vector<Unit> units;
    uint64_t running_offset = 0;  // byte offset within the (page-concatenated) document

    for (const auto& page : input.pages) {
        ++stats->total_pages;
        const uint32_t page_num = page.page_num >= 0 ? static_cast<uint32_t>(page.page_num) : 0;

        // A page with no usable text (e.g. the parser reported it failed → empty
        // page_text + no paragraphs) is counted as a failed page and skipped
        // (per-page tolerance inherited from the parser).
        bool page_had_content = false;

        auto emit = [&](const std::string& raw) {
            std::string text = Trim(raw);
            if (text.empty()) return;
            page_had_content = true;
            Unit u;
            u.token_count = token_counter_(text);
            u.page_num = page_num;
            u.byte_offset = running_offset;
            running_offset += text.size();
            u.text = std::move(text);
            units.push_back(std::move(u));
        };

        switch (config_.unit_level) {
            case UnitLevel::kPage:
                emit(page.page_text);
                break;
            case UnitLevel::kSentence: {
                // Sentence granularity: split the page text on sentence
                // separators via RecursiveChunker at child_size, then each piece
                // is a unit. (A later doc-type classifier auto-detects unit_level; here it is a
                // GUC-selected granularity.)
                RecursiveChunker rc(ChunkConfig{config_.child_size, 0});
                for (const auto& cr : rc.Chunk(page.page_text)) emit(cr.text);
                break;
            }
            case UnitLevel::kParagraph:
            default:
                if (page.paragraphs.empty()) {
                    emit(page.page_text);  // no paragraph structure → whole page
                } else {
                    for (const auto& para : page.paragraphs) emit(para.text);
                }
                break;
        }

        if (page_had_content) {
            ++stats->succeeded_pages;
        } else {
            ++stats->failed_pages;
        }
    }
    return units;
}

std::vector<ParentChildChunker::ChildSpan> ParentChildChunker::SplitIntoChildren(
    const std::string& parent_text) const {
    // Reuse the project's RecursiveChunker (spc) for token-aware, UTF-8-safe child
    // splitting with overlap — it already implements the separator cascade
    // (paragraph → sentence → word → char) + token-based overlap this chunker needs.
    ChunkConfig cfg;
    cfg.chunk_size = config_.child_size;
    cfg.chunk_overlap = config_.child_overlap;
    auto raw = RecursiveChunker(cfg).Chunk(parent_text);

    std::vector<ChildSpan> spans;
    spans.reserve(raw.size());
    size_t search_from = 0;
    for (const auto& cr : raw) {
        if (cr.text.empty()) continue;
        // parent_offset = byte offset of this child inside parent_text (used for
        // context reverse-lookup, § 2.2). Recover via find; fall back to the
        // running cursor if the overlap-prefixed text is not found verbatim.
        size_t pos = parent_text.find(cr.text, search_from);
        if (pos == std::string::npos) pos = search_from <= parent_text.size() ? search_from : 0;
        ChildSpan span;
        span.text = cr.text;
        span.parent_offset = static_cast<uint32_t>(pos);
        span.token_count = token_counter_(cr.text);  // tokenizer-consistent count
        spans.push_back(std::move(span));
        search_from = pos + 1;
    }
    return spans;
}

void ParentChildChunker::BuildParentChild(const ChunkerInput& input,
                                          const std::vector<Unit>& units,
                                          int64_t now_ms, ChunkerOutput* output) const {
    uint32_t chunk_index = 0;  // child order within the doc (0,1,2,...)

    // Accumulate units into a parent until adding the next unit would exceed
    // parent_size; then flush. A single oversized unit still forms its own parent
    // (its children get split down by SplitIntoChildren). (§ 4.1)
    size_t i = 0;
    const size_t n = units.size();
    while (i < n) {
        std::string parent_text;
        uint32_t parent_tokens = 0;
        uint32_t page_start = units[i].page_num;
        uint32_t page_end = units[i].page_num;
        uint64_t byte_start = units[i].byte_offset;
        uint64_t byte_end = units[i].byte_offset;

        while (i < n) {
            const Unit& u = units[i];
            // Flush boundary: appending u would overflow parent_size, and we
            // already have content. (If parent is empty we always take at least
            // one unit, even if it alone exceeds parent_size.)
            if (!parent_text.empty() && parent_tokens + u.token_count > static_cast<uint32_t>(config_.parent_size)) {
                break;
            }
            if (!parent_text.empty()) parent_text += "\n";
            parent_text += u.text;
            parent_tokens += u.token_count;
            page_end = u.page_num;
            byte_end = u.byte_offset + u.text.size();
            ++i;
        }

        if (parent_text.empty()) { ++output->stats.empty_parents; continue; }

        ParentChunk parent;
        parent.parent_id = cortrix::id::GenerateUlid(now_ms);
        parent.doc_id = input.doc_id;
        parent.namespace_id = input.namespace_id;
        parent.parent_text = parent_text;
        parent.token_count = token_counter_(parent_text);
        parent.page_start = page_start;
        parent.page_end = page_end;
        parent.byte_offset_start = byte_start;
        parent.byte_offset_end = byte_end;
        parent.metadata = input.metadata;  // inherit doc-level (§ 3.2)
        parent.created_at = now_ms;

        // Split this parent into children.
        for (const auto& span : SplitIntoChildren(parent_text)) {
            ChildChunk child;
            child.child_id = cortrix::id::GenerateUlid(now_ms);
            child.parent_id = parent.parent_id;     // FK → parent (§ 3.2)
            child.doc_id = input.doc_id;
            child.namespace_id = input.namespace_id;
            child.child_text = span.text;
            child.token_count = span.token_count;
            child.parent_offset = span.parent_offset;
            child.chunk_index = chunk_index++;
            child.metadata = parent.metadata;       // child inherits parent metadata (D9)
            child.created_at = now_ms;
            parent.child_ids.push_back(child.child_id);
            output->children.push_back(std::move(child));
        }

        output->parents.push_back(std::move(parent));
    }
}

void ParentChildChunker::BuildFlat(const ChunkerInput& input,
                                   const std::vector<Unit>& units,
                                   int64_t now_ms, ChunkerOutput* output) const {
    // Flat fallback (§ 4.5 / D4 lock): the whole document becomes one flat layer
    // of child-sized chunks with NO parent-child relationship. We concatenate all
    // unit text and split it into children; each child's parent_id stays empty.
    std::string full;
    for (const auto& u : units) {
        if (!full.empty()) full += "\n";
        full += u.text;
    }

    uint32_t chunk_index = 0;
    for (const auto& span : SplitIntoChildren(full)) {
        ChildChunk child;
        child.child_id = cortrix::id::GenerateUlid(now_ms);
        child.parent_id = "";  // flat: no parent
        child.doc_id = input.doc_id;
        child.namespace_id = input.namespace_id;
        child.child_text = span.text;
        child.token_count = span.token_count;
        child.parent_offset = span.parent_offset;
        child.chunk_index = chunk_index++;
        child.metadata = input.metadata;
        child.created_at = now_ms;
        output->children.push_back(std::move(child));
    }
    output->stats.fallback_to_flat = true;
}

Result<ChunkerOutput> ParentChildChunker::Chunk(const ChunkerInput& input) {
    const int64_t now_ms = NowMs();
    ChunkerOutput output;

    std::vector<Unit> units = CollectUnits(input, &output.stats);

    // Empty document (§ 4.5 → CX_ERR_CHUNK_EMPTY_DOCUMENT): no usable page/text.
    if (units.empty()) {
        return ChunkStatus(
            StatusCode::kInvalidArgument, chunk_errors::kEmptyDocument,
            "document '" + input.doc_id + "' produced no usable chunks (total_pages=" +
                std::to_string(output.stats.total_pages) + ", failed_pages=" +
                std::to_string(output.stats.failed_pages) + ")");
    }

    // Estimate parent count to decide flat fallback (§ 4.5). An upper bound on the
    // number of parents = total_tokens / parent_size (each parent holds ≥ ~1
    // parent_size worth of units except possibly the last), rounded up; this is
    // conservative and avoids building the full tree before deciding.
    uint64_t total_tokens = 0;
    for (const auto& u : units) total_tokens += u.token_count;
    const uint32_t psize = std::max(1, config_.parent_size);
    const uint64_t est_parents = (total_tokens + psize - 1) / psize;

    if (config_.strategy == ChunkStrategy::kFlat ||
        est_parents > static_cast<uint64_t>(config_.fallback_to_flat_threshold)) {
        BuildFlat(input, units, now_ms, &output);
        // CX_WARN_CHUNK_FALLBACK is non-blocking; the boundary surfaces it into
        // meta.warnings with structured_data {doc_id, parent_count, threshold}.
        if (config_.strategy != ChunkStrategy::kFlat) {
            output.stats.fallback_to_flat = true;
        }
    } else {
        BuildParentChild(input, units, now_ms, &output);

        // Overflow guard: even after building, if the realized parent count blew
        // past the cap AND flat fallback is somehow unavailable, that is a hard
        // error. With D4's fallback == threshold this is unreachable in practice
        // (we'd have taken the flat path above), but we keep the gate for the
        // case strategy=parent-child with fallback disabled below the cap.
        if (output.parents.size() > static_cast<size_t>(config_.max_parents_per_doc)) {
            return ChunkStatus(
                StatusCode::kInvalidArgument, chunk_errors::kOverflow,
                "document '" + input.doc_id + "' produced " +
                    std::to_string(output.parents.size()) + " parents > max_parents_per_doc " +
                    std::to_string(config_.max_parents_per_doc) + " and flat fallback failed");
        }
    }

    output.stats.total_parents = static_cast<uint32_t>(output.parents.size());
    output.stats.total_children = static_cast<uint32_t>(output.children.size());
    return output;
}

}  // namespace cortrix::chunker
