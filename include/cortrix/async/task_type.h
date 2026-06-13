#pragma once

namespace cortrix::async {

/// TaskType — Phase 1 async-task-type enum (F42 §3.2, **SoT**).
///
/// F42 is the Phase 1 async-task-scheduling owner Feature, so the TaskType enum
/// is declared here once; F06/F08/F21/F41 reverse-reference this header rather
/// than declaring their own (V2 QA M-17: F41 §5.2 previously declared it
/// unilaterally, violating SoT ownership).
///
/// Enum-value stability contract (F42 §3.2):
///  - The values (1/2/3) are stable across Phases so the tasks.task_type column
///    stays backward-compatible.
///  - A new TaskType MUST be added by a PR that also updates F42 §3.2 + ARCH
///    §2.10 (no Feature may declare one unilaterally).
enum TaskType {
    kTaskDocParse        = 1,   ///< F06/F08 established (PDF/Office parse task)
    kTaskWatcherFanout   = 2,   ///< F21 established (Watcher cross-NS broadcast task)
    kTaskDocSummary      = 3,   ///< F41 added (document-level summary index generation task)
};

}  // namespace cortrix::async
