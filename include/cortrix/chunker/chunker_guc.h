#pragma once
#include <string>

#include "cortrix/chunker/chunker_config.h"
#include "cortrix/common/i_global_config.h"
#include "cortrix/common/result.h"
#include "cortrix/common/status.h"

// F34 chunker GUC keys + range validation (detailed design § 2.3 GUC table SoT in code).
//
// Standalone (D3): defines the GUC names + [min,max] ranges, pure validation, and
// a load-from-IGlobalConfig path. Registering with the live PostgreSQL GUC
// machinery (DefineCustomIntVariable etc.) is pgcortrix / server integration →
// D3.5; here the validated values flow into ChunkerConfig for ParentChildChunker.
//
// Range-violation policy: REJECT (return Status error), not silent clamp — a GUC
// outside its documented range is an operator error surfaced at config load
// (mirrors reranker_guc REJECT policy).
namespace cortrix::chunker {

namespace guc {

// Keys (detailed design § 2.3). child_size is ALSO bounded at startup by
// reranker.max_seq_length (F34-1, enforced in chunker_startup_validator).
inline constexpr const char* kStrategy                 = "spc.chunker.strategy";
inline constexpr const char* kParentSize               = "spc.chunker.parent_size";
inline constexpr const char* kChildSize                = "spc.chunker.child_size";
inline constexpr const char* kChildOverlap             = "spc.chunker.child_overlap";
inline constexpr const char* kUnitLevel                = "spc.chunker.unit_level";
inline constexpr const char* kMaxParentsPerDoc         = "spc.chunker.max_parents_per_doc";
inline constexpr const char* kFallbackToFlatThreshold  = "spc.chunker.fallback_to_flat_threshold";
inline constexpr const char* kChildrenPerParentRerank  = "spc.chunker.children_per_parent_for_rerank";

// Ranges (detailed design § 2.3). Inclusive [min, max].
inline constexpr int kParentSizeMin = 256,  kParentSizeMax = 8192;
inline constexpr int kChildSizeMin = 50,    kChildSizeMax = 8192;   ///< upper also ≤ max_seq_length (F34-1, checked separately)
inline constexpr int kChildOverlapMin = 0,  kChildOverlapMax = 100;
inline constexpr int kMaxParentsMin = 100,  kMaxParentsMax = 100000;
inline constexpr int kChildrenPerParentMin = 1, kChildrenPerParentMax = 10;

}  // namespace guc

class ChunkerGuc {
public:
    /// Validate every ranged field of `config` against its § 2.3 range, plus the
    /// cross-field invariant child_size ≤ parent_size. Returns OK or the first
    /// offending field's Status (InvalidArgument, message names the field + value
    /// + range). Does NOT check child_size ≤ max_seq_length — that needs the
    /// reranker config and lives in ChunkerStartupValidator (F34-1).
    static Status ValidateConfig(const ChunkerConfig& config);

    /// Validate a single int value against [min, max]; OK or InvalidArgument
    /// naming `key`, the value, and the range. Exposed for per-GUC set-time checks.
    static Status ValidateRange(const std::string& key, int value, int min, int max);

    /// Read spc.chunker.* keys from `cfg` into `out` (keeping the struct default
    /// when a key is absent), then ValidateConfig(*out). Returns OK or the
    /// validation error. Live PostgreSQL GUC registration = D3.5; this is the
    /// standalone load path used by tests + the resolver baseline.
    static Status LoadFromConfig(const IGlobalConfig& cfg, ChunkerConfig* out);
};

}  // namespace cortrix::chunker
