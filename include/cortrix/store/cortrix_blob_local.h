#pragma once
#include <cstdint>
#include "cortrix/store/cortrix_blob_store.h"
#include <string>

namespace cortrix {

class CortrixBlobLocal : public CortrixBlobStore {
public:
    explicit CortrixBlobLocal(const std::string& base_dir);

    int store(const std::string& ns, const std::string& doc_id,
              const void* data, size_t len) override;
    int load(const std::string& ns, const std::string& doc_id,
             std::vector<uint8_t>& out) override;
    int del(const std::string& ns, const std::string& doc_id) override;

private:
    /// Generate path: base_dir/ns/blobs/{doc_id%256}/{doc_id}.raw
    std::string MakePath(const std::string& ns, const std::string& doc_id) const;

    std::string base_dir_;
};

}  // namespace cortrix
