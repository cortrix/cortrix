#pragma once
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "cortrix/retrieval/splade_sparse_retriever.h"

namespace cortrix::retrieval {

/// SparseIndexRegistry — the D3.5 per-NS lifecycle owner for the F40 SPLADE
/// inverted index (Q4 wiring).
///
/// The frozen SpladeSparseRetriever owns one SQLite handle (its inverted index);
/// F40's design leaves "injecting the per-NS connection" to D3.5. This registry is
/// that injection: it lazily opens ONE retriever per namespace (keyed by ns_id) and
/// hands the SAME instance to both the ingest write path (Add at chunk embed) and
/// the query read path (Search), so writes and reads share one index. Each NS's
/// index is a file under the NS data dir (<data_dir>/<ns>/sparse_index.db) when a
/// data_dir is configured, else an in-process ":memory:" index (tests / ephemeral).
///
/// Thread-safe: GetOrOpen takes a mutex around the per-NS map; the returned
/// retriever is itself internally synchronized (SpladeSparseRetriever holds its own
/// mutex), so concurrent Search/Add on one NS are safe.
class SparseIndexRegistry {
public:
    /// @param data_dir  NS data root; "" → every NS index is ":memory:". The per-NS
    ///                 path is <data_dir>/<ns_id>/sparse_index.db.
    /// @param config    SpladeConfig applied to every per-NS retriever (top-K + caps).
    explicit SparseIndexRegistry(std::string data_dir, SpladeConfig config = {});

    /// Return the retriever for `ns_id`, opening + Open()-ing it on first use.
    /// Returns nullptr only if the index could not be opened (the caller then
    /// treats sparse as unavailable → L2 fallback, §7.2). Never throws.
    ISparseRetriever* GetOrOpen(const std::string& ns_id);

private:
    std::string PathFor(const std::string& ns_id) const;

    std::string data_dir_;
    SpladeConfig config_;
    std::mutex mu_;
    std::map<std::string, std::unique_ptr<SpladeSparseRetriever>> by_ns_;  // guarded by mu_
};

}  // namespace cortrix::retrieval
