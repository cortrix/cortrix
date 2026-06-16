#include <cstdint>
#include "cortrix/store/cortrix_blob_local.h"
#include "cortrix/logging/logging.h"

#include <fstream>
#include <filesystem>
#include <cstdio>
#include <functional>  // std::hash for ULID doc_id bucketing (D-I6)

namespace fs = std::filesystem;

namespace cortrix {

CortrixBlobLocal::CortrixBlobLocal(const std::string& base_dir)
    : base_dir_(base_dir) {}

std::string CortrixBlobLocal::MakePath(const std::string& ns, const std::string& doc_id) const {
    // base_dir/ns_<ns>/blobs/{hash(doc_id)%256}/{doc_id}.raw  (D-I6: doc_id is a
    // ULID string; bucket by its hash, filename is the ULID itself — base32,
    // filesystem-safe).
    int bucket = static_cast<int>(std::hash<std::string>{}(doc_id) % 256);
    return base_dir_ + "/ns_" + ns + "/blobs/" +
           std::to_string(bucket) + "/" +
           doc_id + ".raw";
}

int CortrixBlobLocal::store(const std::string& ns, const std::string& doc_id,
                            const void* data, size_t len) {
    std::string path = MakePath(ns, doc_id);

    // Create directory
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    if (ec) {
        CORTRIX_LOG_ERROR("blob", "mkdir failed: {} - {}", path, ec.message());
        return -1;
    }

    // Atomic write: write to tmp, then rename
    std::string tmp_path = path + ".tmp";
    {
        std::ofstream out(tmp_path, std::ios::binary);
        if (!out) {
            CORTRIX_LOG_ERROR("blob", "Cannot create temp file: {}", tmp_path);
            return -1;
        }
        out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(len));
        if (!out) {
            CORTRIX_LOG_ERROR("blob", "Write failed: {}", tmp_path);
            return -1;
        }
        out.flush();
    }

    // Rename for atomicity
    ec.clear();
    fs::rename(tmp_path, path, ec);
    if (ec) {
        CORTRIX_LOG_ERROR("blob", "rename failed: {} -> {} - {}", tmp_path, path, ec.message());
        std::remove(tmp_path.c_str());
        return -1;
    }

    return 0;
}

int CortrixBlobLocal::load(const std::string& ns, const std::string& doc_id,
                            std::vector<uint8_t>& out) {
    std::string path = MakePath(ns, doc_id);

    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        return -2;  // not found
    }

    auto size = in.tellg();
    if (size <= 0) {
        out.clear();
        return 0;
    }

    in.seekg(0);
    out.resize(static_cast<size_t>(size));
    in.read(reinterpret_cast<char*>(out.data()), size);
    if (!in) {
        CORTRIX_LOG_ERROR("blob", "Read failed: {}", path);
        return -1;
    }

    return 0;
}

int CortrixBlobLocal::del(const std::string& ns, const std::string& doc_id) {
    std::string path = MakePath(ns, doc_id);

    std::error_code ec;
    if (!fs::exists(path, ec)) {
        return -2;  // not found
    }

    if (!fs::remove(path, ec)) {
        CORTRIX_LOG_ERROR("blob", "delete failed: {} - {}", path, ec.message());
        return -1;
    }

    return 0;
}

}  // namespace cortrix
