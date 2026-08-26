#include "cortrix/async/managed_input.h"

#include <filesystem>

namespace cortrix::async {

bool ReleaseManagedPath(const std::string& managed_dir, const std::string& filepath,
                        const std::string& exclude_task_id, TaskManager* mgr) {
    if (managed_dir.empty() || filepath.empty() || mgr == nullptr) return false;

    std::error_code ec;
    // Resolve both sides before comparing so ".." segments and symlinks cannot
    // make a caller-owned path look like it lives in the managed dir.
    const std::filesystem::path dir = std::filesystem::weakly_canonical(managed_dir, ec);
    if (ec) return false;
    const std::filesystem::path path = std::filesystem::weakly_canonical(filepath, ec);
    if (ec) return false;
    // Rule 1 — managed inputs are flat in the dir; anything nested or elsewhere is
    // not ours.
    if (path.parent_path() != dir) return false;

    // Rules 2 and 3 — never remove a path another live task still needs, and treat
    // an unanswerable check as "still referenced".
    //
    // The comparison is on resolved paths, not stored strings: task rows hold
    // whatever was handed to F42, so the same file can appear as "d/x.txt" for one
    // task and "d/./x.txt" for another. That identity is resolved once at task
    // creation and indexed (tasks.filepath_canonical), so this is a point lookup
    // rather than resolving every live row on every completion — the O(live tasks)
    // scan that gated the whole worker pool at depth (issue #74).
    //
    // The check and the removal happen under one lock, so a task created in between
    // cannot have its input deleted out from under it.
    auto released = mgr->ReleaseInputIfUnreferenced(
        path.string(), exclude_task_id, [&path]() {
            std::error_code rec;
            return std::filesystem::remove(path, rec) && !rec;
        });
    return released.ok() && released.value();
}

}  // namespace cortrix::async
