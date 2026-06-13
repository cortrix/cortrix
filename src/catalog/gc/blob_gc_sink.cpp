#include "cortrix/catalog/gc/blob_gc_sink.h"

#include <filesystem>
#include <utility>

namespace fs = std::filesystem;

namespace cortrix::catalog::gc {

LocalBlobGcSink::LocalBlobGcSink(std::string base_dir) : base_dir_(std::move(base_dir)) {}

Status LocalBlobGcSink::Unlink(const std::string& blob_uri) {
    // blob_uri is the content-addressed relative path ({hash[0:2]}/{hash}); resolve
    // it under base_dir_ and remove. A missing file is success (idempotent Stage 3
    // re-run / a blob never materialized) — only a real fs error is reported so the
    // queue row stays pending for the next sweep.
    std::error_code ec;
    const fs::path path = fs::path(base_dir_) / blob_uri;
    fs::remove(path, ec);
    if (ec) {
        return Status::Internal("CX_ERR_GC_BLOB_UNLINK_FAILED: " + path.string() +
                                ": " + ec.message());
    }
    return Status::Ok();
}

}  // namespace cortrix::catalog::gc
