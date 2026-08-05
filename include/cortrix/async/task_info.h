#pragma once
#include <string>
#include <vector>

#include "cortrix/async/task_type.h"

namespace cortrix::async {

/// Task status string constants (F42 §3.1 status state machine). The tasks table
/// CHECK constraint enforces exactly this set.
///   queued → processing → completed
///                      ↘ failed
///          ↗ cancelling → cancelled
namespace task_status {
inline constexpr const char* kQueued     = "queued";
inline constexpr const char* kProcessing = "processing";
inline constexpr const char* kCancelling = "cancelling";
inline constexpr const char* kCompleted  = "completed";
inline constexpr const char* kFailed     = "failed";
inline constexpr const char* kCancelled  = "cancelled";
}  // namespace task_status

/// current_phase string constants (F42 §3.1 / topic 5 Progress meta).
namespace task_phase {
inline constexpr const char* kParsing   = "parsing";
inline constexpr const char* kEnriching = "enriching";
inline constexpr const char* kIndexing  = "indexing";
}  // namespace task_phase

/// A row of the tasks table (F42 §3.1 + §3.3). The persisted unit of async
/// document processing. `failed_pages` is stored as a JSON int array in the DB
/// and materialized here as a vector<int>.
struct TaskInfo {
    std::string task_id;             ///< ULID PRIMARY KEY (id/ulid.h)
    std::string namespace_id;
    std::string filename;
    std::string filepath;            ///< Temporary file path
    std::string doc_id;              ///< Filled in after completion (Phase 1 inferred from content_hash)
    std::string content_hash;        ///< topic 2.2 — used for Watcher debounce/dedup
    std::string status = task_status::kQueued;
    int task_type = kTaskDocParse;   ///< async::TaskType value (default doc parse)
    bool cancel_requested = false;   ///< topic 3 — cancel-request flag (DB 0/1)
    int total_pages = 0;
    int processed_pages = 0;
    std::vector<int> failed_pages;   ///< Q1 per-page fault tolerance (DB JSON array)
    float progress_pct = 0.0f;
    int eta_seconds = -1;
    std::string current_phase;       ///< topic 5 — parsing | enriching | indexing
    int worker_id = -1;              ///< topic 5 — which Worker is processing (-1 = unassigned)
    std::string trace_id;            ///< topic 6 — cross-Worker trace (filled at D3 implementation)
    std::string error_code;          ///< topic 5 — GEN-Agent CX_ERR_* code
    std::string error_msg;
    std::string structured_data;     ///< topic 5 — GEN-Agent JSON error context (serialized string)
    std::string created_at;          ///< queued time (ISO8601)
    std::string updated_at;
    std::string started_at;          ///< actual processing start time
    std::string completed_at;
    std::string metadata_json;       ///< caller-supplied document metadata (JSON object string); rides to the doc row so it round-trips to query results
};

/// Submission request handed to TaskScheduler::Enqueue / TaskManager::CreateTask
///. The caller (HTTP handler) fills it from the upload request.
struct SubmitRequest {
    std::string namespace_id;
    std::string filename;
    std::string filepath;
    std::string doc_id;          ///< Phase 1: content-hash-derived doc identity
    std::string content_hash;    ///< topic 2.2 debounce/dedup
    std::string trace_id;        ///< topic 6 — extracted from the traceparent header at D3 implementation
    std::string metadata_json;   ///< caller-supplied document metadata (JSON object string); persisted on the task so it survives the queue to the doc row
    int total_pages = 0;         ///< total page count estimated by pre-check
    int task_type = 1;           ///< async::TaskType value; default kTaskDocParse
};

}  // namespace cortrix::async
