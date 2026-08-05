#pragma once
#include <string>

#include "cortrix/async/task_manager.h"
#include "cortrix/common/result.h"

namespace cortrix::server {

/// Sole owner of the "batch materialized input" on-disk concept.
///
/// POST /documents/batch carries inline content, but the doc-parse worker
/// reads a *file* (DocumentParserFactory::ParseDocument(filepath)). The batch
/// service therefore materializes each accepted doc under this directory and
/// hands that path to the scheduler as SubmitRequest.filepath.
///
/// The writer (BatchSubmitService) and both reapers (TaskFinalizer's terminal
/// release + the startup orphan sweep) resolve the directory through
/// BatchTempDir(), so the location can never drift between them.

/// `<data_dir>/batch_tmp` — where batch inline content is materialized.
std::string BatchTempDir(const std::string& data_dir);

/// Delete regular files directly under `dir` that no live (non-terminal) task
/// still references. Returns the number of files deleted; a missing directory
/// is success (0).
///
/// Why this is needed *in addition to* TaskFinalizer's per-task release:
///   - SweepZombies / SweepTimedOut move rows to `failed` with a bulk UPDATE
///     that never reaches a handler, so those tasks have no terminal hook and
///     would otherwise retain their input forever;
///   - a crash between materialization and the terminal write leaves a file
///     with no live owner;
///   - the debounce refresh (TaskManager::UpdateTaskForDebounce) repoints
///     tasks.filepath at a new file, orphaning the superseded one;
///   - pre-existing files from before the release hook existed.
///
/// Contract: call at startup, after any re-queue of non-terminal rows and
/// before the batch route is mounted. It reads the live task table, so a
/// queued/processing task's input is never removed; as defense in depth against
/// being called while submissions are in flight, files modified within
/// kOrphanGraceSeconds are also skipped.
Result<int> SweepOrphanedBatchInputs(const std::string& dir, async::TaskManager* mgr);

/// Recently-modified files are left alone by the sweep (see above).
inline constexpr int kOrphanGraceSeconds = 300;

}  // namespace cortrix::server
