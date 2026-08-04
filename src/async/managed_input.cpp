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
    // an unanswerable check as "still referenced". Pass the ORIGINAL filepath, not
    // the resolved one: the task rows store what was handed to F42.
    auto others = mgr->CountOtherLiveTasksWithFilepath(filepath, exclude_task_id);
    if (!others.ok() || others.value() > 0) return false;

    return std::filesystem::remove(path, ec) && !ec;
}

}  // namespace cortrix::async
