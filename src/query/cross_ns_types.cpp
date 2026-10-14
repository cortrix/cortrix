#include "cortrix/retrieval/cross_ns_types.h"

namespace cortrix::retrieval {

ResultItem ToResultItem(const RankedChunk& rc,
                        const std::string& ns_id,
                        const std::string& content_hash_str) {
    ResultItem item;
    item.child_id = rc.child_id;
    // SPEC §1 v1.2: RankedChunk now carries the owning parent ULID, so this V5
    // ruling #6A primary identifier is sourced from the hit instead of defaulted.
    // Empty when the hit is not a child block (e.g. a doc-level pseudo-result).
    item.parent_id = rc.parent_id;
    item.namespace_id = ns_id;
    item.content = rc.chunk_text;
    item.parent_content = rc.parent_text;
    item.score = rc.score;
    item.rerank_score = rc.rerank_score;
    item.score_signals = rc.score_signals;
    item.content_hash = content_hash_str;
    item.metadata = rc.metadata;
    return item;
}

}  // namespace cortrix::retrieval
