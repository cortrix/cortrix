#include "cortrix/server/batch_temp_store.h"

#include <chrono>
#include <filesystem>
#include <string>
#include <unordered_set>

#include <spdlog/spdlog.h>

namespace fs = std::filesystem;

namespace cortrix::server {

std::string BatchTempDir(const std::string& data_dir) {
    return data_dir + "/batch_tmp";
}

Result<int> SweepOrphanedBatchInputs(const std::string& dir, async::TaskManager* mgr) {
    if (dir.empty() || mgr == nullptr) return 0;

    std::error_code ec;
    if (!fs::exists(dir, ec) || ec) return 0;  // nothing materialized yet

    // Live set = every filepath still owned by a non-terminal task. Read once,
    // before touching the filesystem, so a row that terminates mid-sweep can
    // only ever cause us to keep a file (reclaimed next start), never delete a
    // live one.
    auto live = mgr->LiveTaskInputs();
    if (!live.ok()) return live.status();
    std::unordered_set<std::string> keep;
    for (const auto& [task_id, fp] : live.value()) {
        (void)task_id;
        if (fp.empty()) continue;
        // Resolve before keying so a live input is recognised however its row
        // happens to spell it — the same identity the release path uses.
        const fs::path canon = fs::weakly_canonical(fp, ec);
        if (ec) {
            ec.clear();
            continue;  // unresolvable → cannot key it; the entry below fails closed
        }
        keep.insert(canon.string());
    }

    // C++17: the filesystem clock is reachable only through file_time_type.
    const auto now = fs::file_time_type::clock::now();
    int deleted = 0;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec) || ec) {
            ec.clear();
            continue;
        }
        const fs::path canon = fs::weakly_canonical(entry.path(), ec);
        const std::string key = ec ? entry.path().string() : canon.string();
        ec.clear();
        if (keep.count(key) > 0) continue;  // a live task still needs this input

        // Grace window: never reclaim a file that was written moments ago. At the
        // startup call site nothing is in flight, but this keeps the sweep safe if
        // it is ever invoked while the batch route is live.
        const auto mtime = fs::last_write_time(entry.path(), ec);
        if (!ec) {
            const auto age = std::chrono::duration_cast<std::chrono::seconds>(now - mtime);
            if (age.count() < kOrphanGraceSeconds) continue;
        }
        ec.clear();

        if (fs::remove(entry.path(), ec) && !ec) ++deleted;
        ec.clear();
    }

    if (deleted > 0) {
        spdlog::info("batch temp sweep: reclaimed {} orphaned input file(s) under {}",
                     deleted, dir);
    }
    return deleted;
}

}  // namespace cortrix::server
