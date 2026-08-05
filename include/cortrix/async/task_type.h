#pragma once

namespace cortrix::async {

/// TaskType — Phase 1 async-task-type enum (**SoT**).
///
/// The async scheduler owns task scheduling in Phase 1, so the TaskType enum
/// is declared here once; its consumers reverse-reference this header rather
/// than declaring their own (the doc-summary design previously declared it
/// unilaterally, violating SoT ownership).
///
/// Enum-value stability contract:
///  - The values (1/2/3) are stable across Phases so the tasks.task_type column
///    stays backward-compatible.
///  - A new TaskType MUST be added by a PR that also updates the design
///    §2.10 (no Feature may declare one unilaterally).
enum TaskType {
    kTaskDocParse        = 1,   ///< PDF/Office parse task
    kTaskWatcherFanout   = 2,   ///< Watcher cross-NS broadcast task
    kTaskDocSummary      = 3,   ///< document-level summary index generation task
    kTaskEnrichBackfill  = 4,   ///< addendum §3.7 added (per-doc chunk-enrichment backfill;
                                ///< payload = namespace_id + doc_id, owed members re-read
                                ///< from enrich_state — idempotent)
};

}  // namespace cortrix::async
