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
    // The comparison resolves BOTH sides. Task rows store whatever string was
    // handed to the scheduler, so the same file can appear as "d/x.txt" for one task and
    // "d/./x.txt" for another; comparing the stored strings would report "nobody
    // else needs it" and delete a live input. The identity used here is the same
    // one used to remove the file below.
    auto live = mgr->LiveTaskInputs();
    if (!live.ok()) return false;
    for (const auto& [task_id, other_path] : live.value()) {
        if (task_id == exclude_task_id) continue;
        std::error_code oec;
        const std::filesystem::path other = std::filesystem::weakly_canonical(other_path, oec);
        if (oec) return false;           // unresolvable → assume it could be this file
        if (other == path) return false;  // still referenced
    }

    return std::filesystem::remove(path, ec) && !ec;
}

}  // namespace cortrix::async
