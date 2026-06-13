#include "cortrix/retrieval/cross_ns_types.h"

namespace cortrix::retrieval {

ResultItem ToResultItem(const RankedChunk& rc,
                        const std::string& ns_id,
                        const std::string& content_hash_str) {
    ResultItem item;
    item.child_id = rc.child_id;
    // parent_id is not on RankedChunk (parent_text is the reverse-looked-up body);
    // it stays empty until F34/F09 wiring (D3.5) supplies the parent ULID.
    item.parent_id = ParentId();
    item.namespace_id = ns_id;
    item.content = rc.chunk_text;
    item.parent_content = rc.parent_text;
    item.score = rc.score;
    item.rerank_score = rc.rerank_score;
    item.content_hash = content_hash_str;
    item.metadata = rc.metadata;
    return item;
}

}  // namespace cortrix::retrieval
