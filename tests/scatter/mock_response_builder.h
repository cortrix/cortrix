#pragma once
#include <string>
#include <vector>

#include "cortrix/query/cross_ns_error.h"
#include "cortrix/retrieval/cross_ns_types.h"

namespace cortrix::query {

/// ScatterMockResponseBuilder — the 6 NamespaceQueryResult factories.
/// Shared test fixtures for the scatter tests (and Phase 2's multi-Unit router).
/// Each models one integration scenario so a ScatterGather/SingleUnitExecutor
/// test can compose multi-NS scenes without hand-building RankedChunks.
class ScatterMockResponseBuilder {
public:
    /// (1) Normal — `top_n` distinct RankedChunks, descending rerank_score so the
    /// gather order is deterministic. child_id/content are "<ns>_c<i>".
    static retrieval::NamespaceQueryResult Normal(const std::string& ns_id, int top_n) {
        retrieval::NamespaceQueryResult r;
        r.namespace_id = ns_id;
        r.latency_ms = 10;
        for (int i = 0; i < top_n; ++i) {
            retrieval::RankedChunk c;
            c.child_id = ns_id + "_c" + std::to_string(i);
            c.chunk_text = ns_id + " chunk " + std::to_string(i);
            c.rerank_score = 0.9f - 0.01f * static_cast<float>(i);  // descending
            c.score = c.rerank_score;
            r.chunks.push_back(std::move(c));
        }
        return r;
    }

    /// (2) Empty — success, zero chunks (Issue 2.4 empty result — counts as succeeded).
    static retrieval::NamespaceQueryResult Empty(const std::string& ns_id) {
        retrieval::NamespaceQueryResult r;
        r.namespace_id = ns_id;
        r.latency_ms = 5;
        return r;  // error_code empty → success, chunks empty
    }

    /// (3) Timeout — per-NS timeout (Issue 2.5). CX_ERR_NS_TIMEOUT, retryable, 1000ms.
    static retrieval::NamespaceQueryResult Timeout(const std::string& ns_id) {
        const auto& info = GetCrossNsErrorInfo(CrossNsErrorCode::kNsTimeout);
        retrieval::NamespaceQueryResult r;
        r.namespace_id = ns_id;
        r.error_code = info.cx_code;
        r.error_category = agent_friendly::ToString(info.category);
        r.retryable = info.retryable;
        r.retry_after_ms = info.retry_after_ms.value_or(0);
        r.latency_ms = 5000;
        return r;
    }

    /// (4) IndexCorrupt — per-NS internal error (Issue 2.4). CX_ERR_INDEX_CORRUPT,
    /// non-retryable, category=permanent.
    static retrieval::NamespaceQueryResult IndexCorrupt(const std::string& ns_id) {
        const auto& info = GetCrossNsErrorInfo(CrossNsErrorCode::kIndexCorrupt);
        retrieval::NamespaceQueryResult r;
        r.namespace_id = ns_id;
        r.error_code = info.cx_code;
        r.error_category = agent_friendly::ToString(info.category);
        r.retryable = info.retryable;
        r.latency_ms = 8;
        return r;
    }

    /// (5) WithDuplicateContent — one chunk whose chunk_text is `content`, so two
    /// NS built with the same `content` collapse to one content_hash (Issue 3.3 cross-NS
    /// shared chunk; the dedup itself is W3). `rerank_score` lets tests pick the primary.
    static retrieval::NamespaceQueryResult WithDuplicateContent(const std::string& ns_id,
                                                                const std::string& content,
                                                                float rerank_score = 0.8f) {
        retrieval::NamespaceQueryResult r;
        r.namespace_id = ns_id;
        r.latency_ms = 10;
        retrieval::RankedChunk c;
        c.child_id = ns_id + "_dup";
        c.chunk_text = content;  // identical text across NS → identical content_hash
        c.rerank_score = rerank_score;
        c.score = c.rerank_score;
        r.chunks.push_back(std::move(c));
        return r;
    }

    /// (6) WithRerankScores — explicit rerank scores (one chunk per score) for
    /// deterministic cross-NS ordering / top_k truncation tests.
    static retrieval::NamespaceQueryResult WithRerankScores(const std::string& ns_id,
                                                            const std::vector<float>& scores) {
        retrieval::NamespaceQueryResult r;
        r.namespace_id = ns_id;
        r.latency_ms = 10;
        int i = 0;
        for (float s : scores) {
            retrieval::RankedChunk c;
            c.child_id = ns_id + "_s" + std::to_string(i++);
            c.chunk_text = ns_id + " score " + std::to_string(s);
            c.rerank_score = s;
            c.score = c.rerank_score;
            r.chunks.push_back(std::move(c));
        }
        return r;
    }
};

}  // namespace cortrix::query
