#pragma once
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "cortrix/common/result.h"

namespace cortrix::reranker {

/// Per-namespace reranker overrides parsed from `namespaces.reranker_config`
/// JSONB (F02 §2.5 / topic 2.2). V1 = the 3 B-class fields; every field is
/// OPTIONAL so an absent key falls back to the global default at resolve time
/// (topic 2.4 "a missing key falls back to the global default"). Unknown keys are ignored (forward-compatible
/// with the Phase-2 `model` / `score_threshold` keys, topic 2.4).
///
/// A=class resident-resource GUCs (workers / queue_size / timeouts / circuit
/// breaker / model paths / max_seq_length) are deliberately NOT here — they are
/// process-global and not NS-overridable (topic 2.1).
struct NsRerankerConfig {
    std::optional<bool> enabled;               ///< NS on/off for reranking
    std::optional<int>  candidate_multiplier;  ///< top_N = min(top_k × this, max_candidates)
    std::optional<int>  max_candidates;        ///< hard cap on top_N

    /// True iff no field is set (empty '{}' blob = inherit global entirely).
    bool empty() const {
        return !enabled && !candidate_multiplier && !max_candidates;
    }

    /// Parse a `reranker_config` JSONB blob. "" or "{}" → all-unset (inherit
    /// global). Unknown keys ignored; a key present with the wrong JSON type is
    /// ignored too (defensive — a malformed value must never crash the query
    /// path, it degrades to the global default). A blob that is valid JSON but
    /// not an object (e.g. "[]" / "5") → CX_ERR (InvalidArgument); a blob that is
    /// not valid JSON → InvalidArgument.
    static Result<NsRerankerConfig> Parse(const std::string& json_blob);
};

}  // namespace cortrix::reranker
