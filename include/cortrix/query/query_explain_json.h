#pragma once

#include <nlohmann/json.hpp>

#include "cortrix/query/llm_rerank_stage.h"
#include "cortrix/query/rag_fusion.h"

namespace cortrix::query {

/// Serializers for the `explain.llm_dependent_features.*` blocks.
///
/// These live here rather than inline in the route so the emitted shape is
/// reachable from a test. The route used to build the JSON by hand and the
/// tests re-declared the same object literal, which meant a change to the
/// route's shape could not fail a test.
///
/// Both blocks are B-class explain state (§2.5 / §8.2): present only when
/// `?explain=true` and the corresponding stage ran.

/// `explain.llm_dependent_features.rag_fusion`.
nlohmann::json BuildRagFusionExplain(const RagFusion::ExplainState& es);

/// `explain.llm_dependent_features.llm_rerank`.
nlohmann::json BuildListwiseRerankExplain(const LlmRerankStage::ExplainState& es);

}  // namespace cortrix::query
