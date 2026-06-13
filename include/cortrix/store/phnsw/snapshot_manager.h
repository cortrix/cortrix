#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "cortrix/common/result.h"
#include "cortrix/common/status.h"

// hnswlib graph type referenced only by pointer here; the include lives in the
// .cpp (mirrors phnsw.h).
namespace hnswlib {
template <typename T>
class HierarchicalNSW;
}

namespace cortrix::store {

/// Manages P-HNSW snapshots under a per-Unit data dir (design § 2.5 / § 4.3).
///
/// A snapshot is a pair of sibling files:
///   hnsw_{timestamp}.index       — hnswlib's native serialization (saveIndex)
///   hnsw_{timestamp}.index.meta  — 24-byte footer (design § 3.3):
///       snapshot_lsn  u64   the committed_lsn captured at snapshot time
///       entry_count   u32   vectors in the snapshot (sanity)
///       dim           u32   index dimension (D35-NEW-01: Open reads this back)
///       M             u32   HNSW graph degree (D35-NEW-01)
///       checksum      u32   CRC32 of the .index file's bytes
///
/// Why a sibling .meta instead of appending the footer to the .index file
/// (as design § 3.3 literally sketches): hnswlib v0.8.0 `loadIndex` seeks to EOF
/// and throws unless `tellg() == total_filesize`, so any trailing footer makes
/// the native load fail. The sibling .meta keeps the exact footer fields +
/// integrity (CRC over the whole .index) while staying compatible with the
/// upstream loader (CODING_CONVENTIONS — normalize spec to the real hnswlib).
///
/// Save is atomic (write temp files, fsync, rename into place). Load picks the
/// newest snapshot whose .meta CRC validates the .index; on a CRC/parse failure
/// it falls back to the previous snapshot (design § 4.4).
class SnapshotManager {
public:
    SnapshotManager(std::string data_dir, int keep_count);

    /// Serialize `index` to a new snapshot stamped with `lsn`. Atomic: writes
    /// hnsw_{ts}.index(.tmp) + .meta(.tmp), fsyncs, renames into place, then
    /// prunes old snapshots beyond keep_count. Returns CX_ERR_PHNSW_SNAPSHOT_*
    /// on failure.
    Status Save(hnswlib::HierarchicalNSW<float>* index, uint64_t lsn, int dim, int M);

    /// Load the newest valid snapshot into `space`-backed `*out_index` (the
    /// caller passes the L2Space the index uses). On success returns the
    /// snapshot's lsn via *out_lsn and the loaded index via *out_index. If no
    /// snapshot exists, returns Ok with *out_index == nullptr and *out_lsn == 0
    /// (caller then starts from an empty index). If every snapshot is corrupt,
    /// returns CX_ERR_PHNSW_SNAPSHOT_CORRUPTED.
    ///
    /// `space` and `max_elements` are needed to construct the hnswlib index.
    Status Load(void* space, int max_elements,
                std::unique_ptr<hnswlib::HierarchicalNSW<float>>* out_index,
                uint64_t* out_lsn);

    /// Delete snapshots beyond the newest keep_count (both .index and .meta).
    /// Also removes any stale .tmp files from interrupted saves.
    void CleanOldSnapshots();

    /// Newest snapshot's lsn, or 0 if none / unreadable. (Diagnostic.)
    uint64_t LatestSnapshotLsn() const;

    /// Footer fields of the newest valid snapshot's .meta (D35-NEW-01). Used by
    /// IIndexFactory::Open to reconstruct the index with the Unit's persisted
    /// graph parameters instead of assuming defaults.
    struct SnapshotMeta {
        uint64_t lsn = 0;
        uint32_t entry_count = 0;
        int dim = 0;
        int M = 0;
    };

    /// Read the newest snapshot's .meta footer (a cheap peek for dim/M before the
    /// index is constructed — no .index CRC check). Returns false if no readable
    /// .meta exists (fresh Unit), in which case the caller falls back to defaults.
    bool PeekLatestMeta(SnapshotMeta* out) const;

private:
    // One discovered snapshot on disk.
    struct SnapshotFile {
        uint64_t timestamp = 0;   ///< parsed from the filename
        std::string index_path;
        std::string meta_path;
    };

    // List snapshots newest-first (by timestamp in the filename).
    std::vector<SnapshotFile> ListSnapshotsNewestFirst() const;

    std::string data_dir_;
    int keep_count_;
};

}  // namespace cortrix::store
