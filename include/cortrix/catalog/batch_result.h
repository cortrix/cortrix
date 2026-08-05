#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace cortrix::catalog {

/// One failed item in a partial-success batch. `cx_code` is the
/// CX_ERR_* string (see catalog_error.h); `detail` is human-readable context.
struct FailedItem {
    std::string id;       // the input id that failed (e.g. ns_id)
    std::string cx_code;  // CX_ERR_* identifying the failure
    std::string detail;
};

/// Batch response metadata (BatchMeta). Carries the 5 standard meta
/// fields (catalog_version / bloom_filter_ready / cache_hit-style flags /
/// timestamp_ms / node_id) + the partial-success accounting. `dedup_applied` is
/// a write-side internal flag (NOT a query-response field).
struct BatchMeta {
    std::vector<std::string> succeeded_ids;
    std::vector<FailedItem>  failed;
    float coverage_ratio = 1.0f;     // succeeded / (succeeded + failed)

    bool    dedup_applied = false;   // write-side internal (see §8.2 note)
    bool    bloom_filter_ready = true;
    int     catalog_version = 0;
    int64_t timestamp_ms = 0;
    std::optional<std::string> node_id;
    std::string api_version = "v1";  // §8.3 versioning promise
};

/// Partial-success batch result. GEN-Agent #3: a batch returns the
/// data it could produce plus structured meta about what failed, rather than
/// all-or-nothing. `results` holds the successful payloads; `meta.failed` the
/// rest.
template <typename T>
struct BatchResult {
    std::vector<T> results;
    BatchMeta meta;
};

}  // namespace cortrix::catalog
