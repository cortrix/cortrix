#include "cortrix/chunker/chunker_guc.h"

#include <string>

namespace cortrix::chunker {

Status ChunkerGuc::ValidateRange(const std::string& key, int value, int min, int max) {
    if (value < min || value > max) {
        return Status::InvalidArgument(
            key + "=" + std::to_string(value) + " out of range [" +
            std::to_string(min) + ", " + std::to_string(max) + "]");
    }
    return Status::Ok();
}

Status ChunkerGuc::ValidateConfig(const ChunkerConfig& config) {
    Status s;
    s = ValidateRange(guc::kParentSize, config.parent_size,
                      guc::kParentSizeMin, guc::kParentSizeMax);
    if (!s.ok()) return s;
    s = ValidateRange(guc::kChildSize, config.child_size,
                      guc::kChildSizeMin, guc::kChildSizeMax);
    if (!s.ok()) return s;
    s = ValidateRange(guc::kChildOverlap, config.child_overlap,
                      guc::kChildOverlapMin, guc::kChildOverlapMax);
    if (!s.ok()) return s;
    s = ValidateRange(guc::kMaxParentsPerDoc, config.max_parents_per_doc,
                      guc::kMaxParentsMin, guc::kMaxParentsMax);
    if (!s.ok()) return s;
    // fallback threshold is locked to = max_parents_per_doc; share its range.
    s = ValidateRange(guc::kFallbackToFlatThreshold, config.fallback_to_flat_threshold,
                      guc::kMaxParentsMin, guc::kMaxParentsMax);
    if (!s.ok()) return s;
    s = ValidateRange(guc::kChildrenPerParentRerank, config.children_per_parent_for_rerank,
                      guc::kChildrenPerParentMin, guc::kChildrenPerParentMax);
    if (!s.ok()) return s;

    // Cross-field invariant: a child cannot be larger than its parent
    // (child is a sub-span of parent_text). The child_size ≤ max_seq_length gate
    // needs the reranker config and lives in ChunkerStartupValidator.
    if (config.child_size > config.parent_size) {
        return Status::InvalidArgument(
            std::string(guc::kChildSize) + "=" + std::to_string(config.child_size) +
            " must be <= " + guc::kParentSize + "=" + std::to_string(config.parent_size));
    }
    return Status::Ok();
}

Status ChunkerGuc::LoadFromConfig(const IGlobalConfig& cfg, ChunkerConfig* out) {
    if (out == nullptr) return Status::InvalidArgument("null ChunkerConfig out");

    // Each key is optional: a present value overrides the struct default; an
    // absent key (Get* not ok) leaves the default in place.
    auto load_int = [&cfg](const char* key, int* field) {
        Result<int> r = cfg.GetInt(key);
        if (r.ok()) *field = r.value();
    };

    load_int(guc::kParentSize, &out->parent_size);
    load_int(guc::kChildSize, &out->child_size);
    load_int(guc::kChildOverlap, &out->child_overlap);
    load_int(guc::kMaxParentsPerDoc, &out->max_parents_per_doc);
    load_int(guc::kFallbackToFlatThreshold, &out->fallback_to_flat_threshold);
    load_int(guc::kChildrenPerParentRerank, &out->children_per_parent_for_rerank);

    Result<std::string> strat = cfg.GetString(guc::kStrategy);
    if (strat.ok()) out->strategy = ChunkStrategyFromString(strat.value());
    Result<std::string> unit = cfg.GetString(guc::kUnitLevel);
    if (unit.ok()) out->unit_level = UnitLevelFromString(unit.value());

    return ValidateConfig(*out);
}

}  // namespace cortrix::chunker
