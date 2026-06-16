#include <cstdint>
#include "cortrix/store/phnsw/snapshot_manager.h"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <utility>

#include "cortrix/logging/logging.h"
#include "cortrix/store/group_commit_writer.h"  // Crc32 (shared)
#include "cortrix/store/phnsw_errors.h"

#include <hnswlib/hnswlib.h>

namespace cortrix::store {

namespace fs = std::filesystem;

namespace {

constexpr char kIndexPrefix[] = "hnsw_";
constexpr char kIndexSuffix[] = ".index";
constexpr char kMetaExt[] = ".meta";  // sibling: hnsw_<ts>.index.meta
constexpr size_t kFooterSize = 24;  // snapshot_lsn(8)+entry_count(4)+dim(4)+M(4)+checksum(4)

void PutU32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>((v >> (8 * i)) & 0xFF);
}
void PutU64(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>((v >> (8 * i)) & 0xFF);
}
uint32_t GetU32(const uint8_t* p) {
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(p[i]) << (8 * i);
    return v;
}
uint64_t GetU64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(p[i]) << (8 * i);
    return v;
}

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// Read an entire file into memory. Returns false on any read error.
bool ReadWholeFile(const std::string& path, std::vector<uint8_t>* out) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in.is_open()) return false;
    const std::streamsize size = in.tellg();
    if (size < 0) return false;
    in.seekg(0, std::ios::beg);
    out->resize(static_cast<size_t>(size));
    if (size > 0 && !in.read(reinterpret_cast<char*>(out->data()), size)) {
        return false;
    }
    return true;
}

// Write bytes to a temp file, fsync, atomically rename to `final_path`. fsync of
// the containing directory (for rename durability) is best-effort.
Status WriteFileAtomic(const std::string& final_path, const uint8_t* data, size_t len,
                       const char* what) {
    const std::string tmp = final_path + ".tmp";
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return PhnswStatus(StatusCode::kUnavailable, phnsw_errors::kSnapshotFailed,
                           std::string(what) + " open tmp: " + std::strerror(errno));
    }
    size_t done = 0;
    while (done < len) {
        ssize_t n = ::write(fd, data + done, len - done);
        if (n < 0) {
            if (errno == EINTR) continue;
            ::close(fd);
            return PhnswStatus(StatusCode::kUnavailable, phnsw_errors::kSnapshotFailed,
                               std::string(what) + " write tmp: " + std::strerror(errno));
        }
        done += static_cast<size_t>(n);
    }
#if defined(__APPLE__)
    if (::fcntl(fd, F_FULLFSYNC) == -1) {
#else
    if (::fsync(fd) != 0) {
#endif
        ::close(fd);
        return PhnswStatus(StatusCode::kUnavailable, phnsw_errors::kSnapshotFailed,
                           std::string(what) + " fsync tmp: " + std::strerror(errno));
    }
    ::close(fd);

    std::error_code ec;
    fs::rename(tmp, final_path, ec);
    if (ec) {
        return PhnswStatus(StatusCode::kUnavailable, phnsw_errors::kSnapshotFailed,
                           std::string(what) + " rename: " + ec.message());
    }
    return Status::Ok();
}

}  // namespace

SnapshotManager::SnapshotManager(std::string data_dir, int keep_count)
    : data_dir_(std::move(data_dir)), keep_count_(keep_count < 1 ? 1 : keep_count) {}

std::vector<SnapshotManager::SnapshotFile> SnapshotManager::ListSnapshotsNewestFirst() const {
    std::vector<SnapshotFile> snaps;
    std::error_code ec;
    if (!fs::exists(data_dir_, ec)) {
        return snaps;
    }
    for (const auto& entry : fs::directory_iterator(data_dir_, ec)) {
        if (ec) break;
        const std::string name = entry.path().filename().string();
        // Match hnsw_<ts>.index  (but NOT hnsw_<ts>.index.meta or *.tmp)
        const size_t plen = std::strlen(kIndexPrefix);
        const size_t slen = std::strlen(kIndexSuffix);
        if (name.size() <= plen + slen) continue;
        if (name.compare(0, plen, kIndexPrefix) != 0) continue;
        if (name.compare(name.size() - slen, slen, kIndexSuffix) != 0) continue;
        // Exclude the .meta sibling (ends with .index.meta, already filtered by
        // the .index suffix check failing) and any .tmp.
        const std::string ts_str =
            name.substr(plen, name.size() - plen - slen);
        if (ts_str.empty() ||
            ts_str.find_first_not_of("0123456789") != std::string::npos) {
            continue;
        }
        SnapshotFile sf;
        sf.timestamp = std::strtoull(ts_str.c_str(), nullptr, 10);
        sf.index_path = entry.path().string();
        sf.meta_path = sf.index_path + kMetaExt;  // hnsw_<ts>.index.meta
        snaps.push_back(std::move(sf));
    }
    std::sort(snaps.begin(), snaps.end(),
              [](const SnapshotFile& a, const SnapshotFile& b) {
                  return a.timestamp > b.timestamp;  // newest first
              });
    return snaps;
}

Status SnapshotManager::Save(hnswlib::HierarchicalNSW<float>* index, uint64_t lsn,
                             int dim, int M) {
    if (index == nullptr) {
        return PhnswStatus(StatusCode::kInvalidArgument, phnsw_errors::kSnapshotFailed,
                           "SnapshotManager::Save: null index");
    }
    std::error_code ec;
    fs::create_directories(data_dir_, ec);

    const uint64_t ts = static_cast<uint64_t>(NowMs());
    const std::string base =
        (fs::path(data_dir_) / (std::string(kIndexPrefix) + std::to_string(ts) + kIndexSuffix))
            .string();
    const std::string index_final = base;
    const std::string index_tmp = base + ".tmp";
    const std::string meta_final = base + kMetaExt;

    // 1. hnswlib serializes to the temp index file (its own native format).
    try {
        index->saveIndex(index_tmp);
    } catch (const std::exception& e) {
        return PhnswStatus(StatusCode::kUnavailable, phnsw_errors::kSnapshotFailed,
                           std::string("SnapshotManager::Save saveIndex: ") + e.what());
    }

    // 2. Read the temp index back to compute its CRC for the footer.
    std::vector<uint8_t> index_bytes;
    if (!ReadWholeFile(index_tmp, &index_bytes)) {
        fs::remove(index_tmp, ec);
        return PhnswStatus(StatusCode::kUnavailable, phnsw_errors::kSnapshotFailed,
                           "SnapshotManager::Save: cannot read temp index for CRC");
    }
    const uint32_t crc = Crc32(index_bytes.data(), index_bytes.size());
    const uint32_t entry_count = static_cast<uint32_t>(index->cur_element_count);

    // 3. fsync the temp index, then rename it into place.
    {
        int fd = ::open(index_tmp.c_str(), O_RDONLY);
        if (fd >= 0) {
#if defined(__APPLE__)
            ::fcntl(fd, F_FULLFSYNC);
#else
            ::fsync(fd);
#endif
            ::close(fd);
        }
    }
    fs::rename(index_tmp, index_final, ec);
    if (ec) {
        fs::remove(index_tmp, ec);
        return PhnswStatus(StatusCode::kUnavailable, phnsw_errors::kSnapshotFailed,
                           "SnapshotManager::Save rename index: " + ec.message());
    }

    // 4. Write the footer/meta atomically. The .meta landing AFTER the .index is
    //    the commit point: Load only trusts a snapshot whose .meta validates, so
    //    a crash between steps 3 and 4 just leaves an orphan .index that Load
    //    skips (no .meta) and CleanOldSnapshots reaps.
    uint8_t footer[kFooterSize];
    PutU64(footer + 0, lsn);
    PutU32(footer + 8, entry_count);
    PutU32(footer + 12, static_cast<uint32_t>(dim));
    PutU32(footer + 16, static_cast<uint32_t>(M));
    PutU32(footer + 20, crc);
    Status mw = WriteFileAtomic(meta_final, footer, kFooterSize, "SnapshotManager::Save meta");
    if (!mw.ok()) {
        // Roll back the orphan index so we don't leave a footerless snapshot.
        fs::remove(index_final, ec);
        return mw;
    }

    CleanOldSnapshots();
    return Status::Ok();
}

Status SnapshotManager::Load(void* space, int max_elements,
                             std::unique_ptr<hnswlib::HierarchicalNSW<float>>* out_index,
                             uint64_t* out_lsn) {
    *out_index = nullptr;
    *out_lsn = 0;

    const std::vector<SnapshotFile> snaps = ListSnapshotsNewestFirst();
    if (snaps.empty()) {
        return Status::Ok();  // no snapshot — caller starts empty
    }

    auto* l2 = static_cast<hnswlib::L2Space*>(space);
    bool any_seen = false;

    for (const SnapshotFile& sf : snaps) {
        // Read footer/meta.
        std::vector<uint8_t> meta;
        if (!ReadWholeFile(sf.meta_path, &meta) || meta.size() != kFooterSize) {
            continue;  // no/short meta → not a committed snapshot, skip
        }
        any_seen = true;
        const uint64_t snap_lsn = GetU64(meta.data() + 0);
        const uint32_t footer_crc = GetU32(meta.data() + 20);

        // Verify the .index CRC against the footer.
        std::vector<uint8_t> index_bytes;
        if (!ReadWholeFile(sf.index_path, &index_bytes)) {
            CORTRIX_LOG_WARN("phnsw", "snapshot {} unreadable; trying older", sf.index_path);
            continue;
        }
        const uint32_t actual_crc = Crc32(index_bytes.data(), index_bytes.size());
        if (actual_crc != footer_crc) {
            CORTRIX_LOG_WARN("phnsw", "snapshot {} CRC mismatch; trying older", sf.index_path);
            continue;
        }

        // CRC ok → load via hnswlib.
        try {
            auto idx = std::make_unique<hnswlib::HierarchicalNSW<float>>(
                l2, sf.index_path, /*nmslib=*/false, static_cast<size_t>(max_elements),
                /*allow_replace_deleted=*/true);
            *out_index = std::move(idx);
            *out_lsn = snap_lsn;
            return Status::Ok();
        } catch (const std::exception& e) {
            CORTRIX_LOG_WARN("phnsw", "snapshot {} load failed ({}); trying older",
                             sf.index_path, e.what());
            continue;
        }
    }

    if (any_seen) {
        // Snapshots existed but none could be loaded → corrupted set.
        return PhnswStatus(StatusCode::kInternal, phnsw_errors::kSnapshotCorrupted,
                           "SnapshotManager::Load: all snapshots corrupt");
    }
    // Only orphan .index files (no valid .meta) → treat as no snapshot.
    return Status::Ok();
}

bool SnapshotManager::PeekLatestMeta(SnapshotMeta* out) const {
    const std::vector<SnapshotFile> snaps = ListSnapshotsNewestFirst();
    for (const SnapshotFile& sf : snaps) {
        std::vector<uint8_t> meta;
        if (!ReadWholeFile(sf.meta_path, &meta) || meta.size() != kFooterSize) {
            continue;  // no/short meta → not a committed snapshot, skip
        }
        out->lsn = GetU64(meta.data() + 0);
        out->entry_count = GetU32(meta.data() + 8);
        out->dim = static_cast<int>(GetU32(meta.data() + 12));
        out->M = static_cast<int>(GetU32(meta.data() + 16));
        return true;
    }
    return false;
}

void SnapshotManager::CleanOldSnapshots() {
    std::error_code ec;

    // 1. Remove stale .tmp files from interrupted saves.
    if (fs::exists(data_dir_, ec)) {
        for (const auto& entry : fs::directory_iterator(data_dir_, ec)) {
            if (ec) break;
            const std::string name = entry.path().filename().string();
            if (name.size() >= 4 && name.compare(name.size() - 4, 4, ".tmp") == 0 &&
                name.compare(0, std::strlen(kIndexPrefix), kIndexPrefix) == 0) {
                fs::remove(entry.path(), ec);
            }
        }
    }

    // 2. Keep the newest keep_count_ snapshots; delete the rest (.index + .meta).
    const std::vector<SnapshotFile> snaps = ListSnapshotsNewestFirst();
    for (size_t i = static_cast<size_t>(keep_count_); i < snaps.size(); ++i) {
        fs::remove(snaps[i].index_path, ec);
        fs::remove(snaps[i].meta_path, ec);
    }
}

uint64_t SnapshotManager::LatestSnapshotLsn() const {
    const std::vector<SnapshotFile> snaps = ListSnapshotsNewestFirst();
    for (const SnapshotFile& sf : snaps) {
        std::vector<uint8_t> meta;
        if (ReadWholeFile(sf.meta_path, &meta) && meta.size() == kFooterSize) {
            return GetU64(meta.data() + 0);
        }
    }
    return 0;
}

}  // namespace cortrix::store
