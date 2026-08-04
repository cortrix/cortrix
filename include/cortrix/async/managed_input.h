#pragma once
#include <string>

#include "cortrix/async/task_info.h"
#include "cortrix/async/task_manager.h"

namespace cortrix::async {

/// Release of a server-materialized task input.
///
/// Some ingest surfaces (today: POST /documents/batch) carry inline content but
/// the doc-parse worker reads a file, so the server writes the content to a file
/// under a directory it owns and hands that path to F42 as SubmitRequest.filepath.
/// The server therefore owns that file's lifetime — unlike a watcher or connector
/// path, which belongs to the caller and must survive forever.
///
/// Every release goes through here so the three safety rules hold in one place:
///
///   1. **Containment** — only files sitting directly in `managed_dir` are ever
///      removed; a caller-owned path elsewhere is left alone.
///   2. **No live reference** — a path is only removed once no OTHER non-terminal
///      task still points at it. Files materialized before the server minted its
///      own names could be shared by two tasks (the name was derived from the
///      caller's doc_id), so completing one task must not pull the input out from
///      under another that is still queued.
///   3. **Fail closed** — if the reference check cannot be answered (storage
///      error), nothing is deleted. A retained file is reclaimed by the next
///      sweep; a wrongly deleted one is an unrecoverable lost document.
///
/// Callers must additionally have recorded the terminal state durably BEFORE
/// releasing: a task whose terminal write was rejected is still live, and its
/// input has to outlive the attempt.

/// Release `filepath` if it is a managed input no live task other than
/// `exclude_task_id` still references. Returns true when a file was removed.
/// A best-effort operation: any failure leaves the file for the orphan sweep.
bool ReleaseManagedPath(const std::string& managed_dir, const std::string& filepath,
                        const std::string& exclude_task_id, TaskManager* mgr);

/// Convenience wrapper for the common "this task is finished with its input" case.
inline bool ReleaseManagedInput(const std::string& managed_dir, const TaskInfo& task,
                                TaskManager* mgr) {
    return ReleaseManagedPath(managed_dir, task.filepath, task.task_id, mgr);
}

}  // namespace cortrix::async
