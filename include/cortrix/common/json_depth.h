#pragma once
// Depth-bounded validation for externally-supplied JSON metadata.
//
// nlohmann::json::dump() (and the destructor of a very deep value) recurse once per
// nesting level with no depth bound, so a deeply-nested user `metadata` object can
// exhaust the stack and crash the process with a SIGSEGV -- which, being a signal
// rather than a C++ exception, no surrounding try/catch can stop. (json::parse is
// iterative and survives; the danger is dump / deep-tree teardown.)
//
// The fix makes "metadata nests no deeper than kMaxMetadataDepth" a system-wide
// invariant by rejecting over-deep metadata at every ingest boundary before it is
// dumped or stored. JsonMaxDepth walks the tree with an explicit stack, so the check
// itself can never overflow regardless of input depth.

#include <algorithm>
#include <cstddef>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "cortrix/agent_friendly/error.h"

namespace cortrix {

/// Maximum nesting depth allowed for user-supplied metadata JSON. Well below the
/// stack-overflow threshold of nlohmann dump()/teardown (empirically ~19k+), and far
/// above any legitimate metadata (real metadata is near-flat). A scalar/empty
/// container is depth 0; {"a":1} is depth 1; {"a":{"b":1}} is depth 2.
inline constexpr int kMaxMetadataDepth = 64;

/// Greatest nesting depth of `j`, computed iteratively (no recursion -- overflow-proof).
/// Stops early once `cap` is exceeded so a pathologically deep input is O(cap), not
/// O(size). Returns the true depth when it does not exceed `cap`, otherwise a value
/// strictly greater than `cap` (the first depth seen past the cap).
inline int JsonMaxDepth(const nlohmann::json& j, int cap = kMaxMetadataDepth) {
    int max_depth = 0;
    // (node, depth-of-node). Only containers are pushed with their child depth.
    std::vector<std::pair<const nlohmann::json*, int>> stack;
    stack.emplace_back(&j, 0);
    while (!stack.empty()) {
        auto [node, depth] = stack.back();
        stack.pop_back();
        if (depth > max_depth) max_depth = depth;
        if (depth > cap) return depth;  // early-out: already over the limit
        if (node->is_object() || node->is_array()) {
            for (auto it = node->begin(); it != node->end(); ++it) {
                stack.emplace_back(&(*it), depth + 1);
            }
        }
    }
    return max_depth;
}

/// True if `j` nests deeper than `max_depth`.
inline bool JsonExceedsMaxDepth(const nlohmann::json& j, int max_depth = kMaxMetadataDepth) {
    return JsonMaxDepth(j, max_depth) > max_depth;
}

/// GEN-Agent error for over-deep metadata (CLAUDE.md Sec.5 #1/#5 -- machine-readable
/// code + structured_data). 422, permanent, not retryable. `actual_depth` is the
/// measured (or capped lower-bound) depth so an Agent can see how far it overshot.
inline agent_friendly::AgentFriendlyError MakeMetadataTooDeepError(int actual_depth) {
    agent_friendly::AgentFriendlyError err;
    err.code = "CX_ERR_METADATA_TOO_DEEP";
    err.message = "metadata nesting exceeds the maximum allowed depth";
    err.retryable = false;
    err.category = agent_friendly::ErrorCategory::kPermanent;
    err.structured_data = nlohmann::json{{"max_depth", kMaxMetadataDepth},
                                         {"actual_depth", actual_depth}};
    return err;
}

}  // namespace cortrix
