#pragma once
#include <optional>
#include <string>
#include <vector>

#include "cortrix/agent_friendly/error.h"
#include "cortrix/async/task_type.h"

namespace cortrix::doc_summary {

/// The 4 structured fields the LLM produces (metadata_json).
struct DocSummaryStructured {
    std::string summary_text;            ///< 200-500 chars (the main embedding field)
    std::vector<std::string> keywords;   ///< 3-8 keywords/phrases (Agent direct-read)
    std::vector<std::string> topics;     ///< 1-3 topic categories (Agent direct-read)
    std::string one_liner;               ///< <= 50 chars (UI display)
};

/// DocSummaryGenerator output. On success `summary` + `embedding` are
/// filled; on failure `error` carries the Agent-friendly CX_ERR_DOCSUMMARY_* identity.
struct GenerationResult {
    bool success = false;
    DocSummaryStructured summary;
    std::vector<float> embedding;        ///< BGE-M3 1024-dim (empty standalone → integration pipeline)
    std::optional<agent_friendly::AgentFriendlyError> error;
    bool is_chunked = false;             ///< true when the map-reduce path ran (long doc)
    bool no_summary_content = false;     ///< true when the doc exists but has no chunks to summarize
};

/// Doc-summary async task payload.
/// (async::TaskType::kTaskDocSummary=3) — reverse-referenced, not redeclared.
struct DocSummaryTask {
    std::string task_id;
    async::TaskType task_type = async::TaskType::kTaskDocSummary;
    std::string doc_id;
    std::string ns_id;
    int attempt_count = 0;
    // Retry policy: 3 retries + exponential backoff; DLQ after 3 (the scheduler owns it).
};

/// doc_summary_status state machine values. String form is what the
/// metadata_json `status` field stores.
enum class DocSummaryStatus { kPending = 0, kGenerated, kFailed };
const char* ToString(DocSummaryStatus status);

/// One doc-discovery hit. via_path distinguishes the two
/// hybrid recall paths. The LLM-summary path carries the 4 structured fields; the
/// FTS5-fallback path carries the metadata fields instead.
struct DocDiscoveryHit {
    std::string doc_id;
    std::string via_path;            ///< "llm_summary" | "fts5_fallback"
    float match_score = 0.0f;
    // llm_summary path:
    std::string summary_text;
    std::vector<std::string> keywords;
    std::vector<std::string> topics;
    std::string one_liner;
    // fts5_fallback path:
    std::string filename;
    std::string doc_title;
};

}  // namespace cortrix::doc_summary
