#pragma once
#include <vector>
#include <string>
#include "cortrix/common/status.h"
#include "cortrix/common/types.h"
#include "cortrix/query/query_pipeline.h"
#include "cortrix/query/query_request.h"
#include "cortrix/query/query_response.h"
#include "cortrix/memory/memory_store.h"
#include "cortrix/memory/memory_config.h"
#include "cortrix/memory_scorer.h"

namespace cortrix {

enum class MemoryScope {
    kSession = 0,    // limited to a specific session (of this user)
    kUser    = 1,    // all sessions of a specific user
    // kAll removed (MEM05): cross-user memory access is prohibited.
    // Phase 2 may add kAdmin=2 (ABI-compatible append); see MEM05 design § ABI note.
};

struct MemorySearchRequest {
    std::string query;                 // search query text (required)
    std::string namespace_name;        // namespace (required)
    MemoryScope scope = MemoryScope::kUser;  // MEM05: default changed kAll -> kUser
    std::string session_id;            // required when scope=kSession
    std::string user_id;               // MEM05: ALWAYS required (memory isolation)
    int top_k = 10;                    // result count (1-100)
    bool include_expired = false;      // include TTL-expired results
    bool include_invalidated = false;  // MEM01 D4: include status=invalidated memories (default false)
    int timeout_ms = 5000;

    /// Validate parameters — user_id is unconditionally required (MEM05).
    Status Validate() const;
};

struct MemorySearchResultItem {
    std::string interaction_id;
    std::string session_id;
    std::string query_text;            // original Q (interaction rows)
    std::string response_text;         // original A (interaction rows)
    std::string query_type;
    // MEM02 fact/preference/event blocks surfaced by the unified read pipeline
    // (§6.5.3): content is the fact statement (no Q/A split), memory_type is
    // fact|preference|event, block_id is the MEM02 ULID. Empty on interaction rows.
    std::string content;
    std::string memory_type;
    std::string block_id;
    float score = 0.0f;               // RRF fusion score (raw_score before decay)
    // MEM01 classified-decay scoring (design § 2.1). Populated only when a
    // MemoryScorer is injected; on the null-scorer fallback path they stay at
    // their defaults (decay_factor=1.0, final_score==score) so callers see raw
    // RRF behavior unchanged.
    double decay_factor = 1.0;        // fact/preference=1.0, event=exp(-lambda*age)
    double final_score = 0.0;         // raw_score * decay_factor (ranking key)
    bool expired = false;              // TTL expired (when include_expired=true)
    std::string created_at;
    std::string metadata_json;
};

struct MemorySearchResponse {
    std::vector<MemorySearchResultItem> results;
    int total_results = 0;
    int64_t latency_ms = 0;
    bool degraded = false;
};

class MemorySearcher {
public:
    /// @param pipeline: F07 QueryPipeline (per-namespace)
    /// @param memory_store: MemoryStore (per-namespace)
    /// @param config: MemoryConfig
    /// @param scorer: MEM01 MemoryScorer (optional). When non-null, search
    ///                results carry classified-decay scoring; when null the
    ///                searcher falls back to raw RRF scoring (pre-MEM01 behavior).
    ///                The full raw_score -> decay wiring against live blocks
    ///                (memory_type / created_at extraction) is D3.5.
    MemorySearcher(QueryPipeline& pipeline,
                   MemoryStore& memory_store,
                   const MemoryConfig& config,
                   MemoryScorer* scorer = nullptr);

    /// Execute memory semantic search (reuses F07 QueryPipeline)
    MemorySearchResponse Search(const MemorySearchRequest& request);

    /// MEM01: accessor for the injected scorer (may be null). Test/debug.
    MemoryScorer* scorer() const { return scorer_; }

private:
    QueryRequest ToQueryRequest(const MemorySearchRequest& request) const;
    static bool IsExpired(const std::string& metadata_json);
    bool MatchScope(const json& metadata,
                    const MemorySearchRequest& request) const;
    MemorySearchResultItem ToMemoryResult(const ResultItem& item) const;

    QueryPipeline& pipeline_;
    MemoryStore& memory_store_;
    MemoryConfig config_;
    MemoryScorer* scorer_ = nullptr;  // MEM01: optional classified-decay scorer

    // Test-only friends for direct private method coverage
    friend class MemorySearcherPrivateTest;
    friend class Mem05IsolationTest;  // MEM05 user-isolation unit tests
};

}  // namespace cortrix
