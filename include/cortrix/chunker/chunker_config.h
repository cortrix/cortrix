#pragma once
#include <string>

// ChunkerConfig — GUC-backed chunking parameters.
//
// Plain value carrier (mirrors reranker/reranker_config.h): default values +
// documented ranges live here; range enforcement is in ChunkerGuc (chunker_guc.h),
// NS-level override resolution is in ChunkerConfigResolver (chunker_config_resolver.h).
namespace cortrix::chunker {

/// Chunking strategy. Phase 1 V1.0 ships only kParentChild; kFlat is the
/// degraded fallback path, kSemantic is a Phase 2 enum placeholder.
enum class ChunkStrategy { kParentChild, kFlat, kSemantic };

/// Granularity of the splitting unit (paragraph). Phase 2
/// DocumentTypeClassifier auto-detects this per-doc.
enum class UnitLevel { kPage, kParagraph, kSentence };

const char* ToString(ChunkStrategy s);
const char* ToString(UnitLevel u);
/// Parse the GUC string form ("parent-child" / "flat" / "semantic",
/// "page"/"paragraph"/"sentence"); unknown → the V1.0 default.
ChunkStrategy ChunkStrategyFromString(const std::string& s);
UnitLevel UnitLevelFromString(const std::string& s);

/// Chunker configuration. Defaults are the locked values
/// (overlap=20 / unit_level=paragraph / max_parents=10000 /
/// children_per_parent_for_rerank=3).
struct ChunkerConfig {
    ChunkStrategy strategy = ChunkStrategy::kParentChild;
    int parent_size = 1024;                 ///< parent target tokens, range [256, 8192]
    int child_size = 200;                   ///< child target tokens, range [50, max_seq_length]
    int child_overlap = 20;                 ///< child overlap tokens, range [0, 100] (lock)
    UnitLevel unit_level = UnitLevel::kParagraph;  ///< lock
    int max_parents_per_doc = 10000;        ///< V1.0 OSS hard cap, range [100, 100000] (lock)
    int fallback_to_flat_threshold = 10000; ///< flat-fallback trigger = cap (lock)
    int children_per_parent_for_rerank = 3; ///< C top-3 dedup, range [1, 10] (lock)
};

}  // namespace cortrix::chunker
