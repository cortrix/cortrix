#pragma once
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "cortrix/catalog/i_content_ref_store.h"
#include "cortrix/common/result.h"
#include "cortrix/common/status.h"

typedef struct sqlite3 sqlite3;

namespace cortrix::catalog {

/// catalog.db-backed IContentRefStore (F12 §3.3 impl, Story S2.3). Borrows the
/// sqlite3 handle owned by CatalogDb. Maintains catalog.content_refs rows (the
/// authoritative per-(file_hash, ns_id, doc_id) records) and keeps the
/// file_locations.ref_count aggregate in sync when that row exists (it is
/// created by the write path, F25; ref counting tolerates its absence).
class DefaultContentRefStore : public IContentRefStore {
public:
    explicit DefaultContentRefStore(sqlite3* db);

    Status IncrementRef(const std::string& file_hash,
                        const std::string& ns_id,
                        const std::string& doc_id) override;
    Result<int> DecrementRef(const std::string& file_hash,
                             const std::string& ns_id,
                             const std::string& doc_id) override;
    Result<int> GetRefCount(const std::string& file_hash) const override;
    Result<std::vector<std::string>> GetGCCandidates(
        int64_t before_timestamp_ms) const override;

private:
    // ref_count read without taking mu_ — for callers that already hold it
    // (e.g. DecrementRef). The public GetRefCount locks then delegates here.
    Result<int> GetRefCountLocked(const std::string& file_hash) const;

    sqlite3* db_;            // borrowed, not owned
    mutable std::mutex mu_;
};

}  // namespace cortrix::catalog
