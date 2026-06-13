#pragma once
#include <string>
#include <vector>

#include "cortrix/catalog/batch_result.h"
#include "cortrix/catalog/catalog_types.h"
#include "cortrix/common/result.h"
#include "cortrix/common/status.h"

namespace cortrix::catalog {

/// NS routing + namespace lifecycle (F12 §3.1). The catalog is the single SoT
/// for NS / Unit metadata; upstream modules (F04 ScatterGather, F25
/// WriteCoordinator, F21 Watcher, pgcortrix) go through this interface rather
/// than touching catalog.db directly (§2 strict dependency inversion).
///
/// Error model (F-FREEZE-1 / CODING_CONVENTIONS §3): value-returning methods use
/// Result<T> (StatusOr); the error path carries a Status whose code identifies a
/// catalog error (CX_ERR_*, see catalog_error.h). Void-fallible methods return
/// plain Status (no Result<void>). The rich Agent-friendly body is built at the
/// boundary via MakeCatalogError(); this interface stays Status/Result<T>.
///
/// Phase 1 semantics: 1 NS = 1 Unit, so LookupUnits returns a single-element
/// vector and GetActiveUnit returns that Unit. Phase 2 (1:N) returns many.
class INSRouter {
public:
    virtual ~INSRouter() = default;

    /// All Units backing `namespace_id`. Phase 1: exactly one (1:1 mapping).
    /// Error: CX_ERR_NS_NOT_FOUND if the NS does not exist.
    virtual Result<std::vector<UnitDescriptor>> LookupUnits(
        const std::string& namespace_id) const = 0;

    /// The NS's current write-target Unit (Phase 1: its sole Unit).
    /// Error: CX_ERR_NS_NOT_FOUND.
    virtual Result<UnitDescriptor> GetActiveUnit(
        const std::string& namespace_id) const = 0;

    /// The NS metadata row, including the 11 *_config blobs (F12 §4.1).
    /// Error: CX_ERR_NS_NOT_FOUND.
    virtual Result<NSMetadata> GetNamespace(
        const std::string& namespace_id) const = 0;

    /// List namespaces (filtered/paginated per `opts`). Returns a BatchResult so
    /// partial visibility (e.g. some NS unauthorized) is reported in meta rather
    /// than failing the whole call (GEN-Agent #3).
    virtual Result<BatchResult<std::string>> ListNamespaces(
        const ListNamespacesOptions& opts = {}) const = 0;

    /// Create a namespace. Void-fallible → Status (F-FREEZE-1: no Result<void>).
    /// Errors: CX_ERR_NS_ALREADY_EXISTS; CX_ERR_NS_QUOTA_EXCEEDED /
    /// CX_ERR_NS_RESOURCE_BUDGET_EXCEEDED / CX_ERR_NS_POOL_INTERNAL from the F05
    /// admission check the implementation performs (F12 §3.1.bis).
    virtual Status CreateNamespace(const NSMetadata& metadata) = 0;

    /// Soft-delete a namespace (status='deleted'). Void-fallible → Status.
    /// The implementation also triggers F05 pool eviction (F12 §3.1.bis).
    /// Error: CX_ERR_NS_NOT_FOUND.
    virtual Status DeleteNamespace(const std::string& namespace_id) = 0;
};

}  // namespace cortrix::catalog
