#pragma once
#include "cortrix/async/task_info.h"
#include "cortrix/common/status.h"

namespace cortrix::async {

/// ITaskHandler — a WorkerPool dispatch target for one TaskType.
///
/// The pool's worker loop routes each dequeued TaskInfo to the handler registered
/// for task.task_type, so new async task types are added by registering another
/// handler — the pool stays open/closed. The two Phase-1 handlers:
///   kTaskDocParse   (1) → async::DocumentProcessor  (parse + finalize)
///   kTaskDocSummary (3) → doc_summary::F41AsyncWorker (summary write)
///
/// ProcessTask runs one task (already marked processing) to its terminal outcome
/// and returns the terminal Status (Ok on success; the CX_ERR_* Status otherwise)
/// so the worker can log / observe and map it to retry / DLQ.
class ITaskHandler {
public:
    virtual ~ITaskHandler() = default;
    virtual Status ProcessTask(const TaskInfo& task) = 0;
};

}  // namespace cortrix::async
