#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace cortrix::catalog {

/// Unit lifecycle state (units.state). Phase 1 every Unit is `active`;
/// the rest are Phase 2 (sealing → sealed → archived) + `migrating` (Phase 2
/// cross-node transient, prereq constraint 7). Stored as the lowercase string.
enum class UnitState {
    kActive,
    kSealing,
    kSealed,
    kArchived,
    kMigrating,
};

const char* ToString(UnitState state);
/// Parse the stored string; unknown → kActive (Phase 1 conservative default).
UnitState UnitStateFromString(const std::string& s);

/// Physical index backing a Unit (units.index_type). Phase 1 = hnsw.
enum class IndexType {
    kHnsw,
    kBruteForce,
    kQuantizedHnsw,
};

const char* ToString(IndexType type);
IndexType IndexTypeFromString(const std::string& s);

/// A Unit's catalog metadata (units row). The 9 reserved Phase 2
/// fields are present so the descriptor is stable across phases; Phase 1 writes
/// them with their defaults and does not act on the seal/owner fields.
struct UnitDescriptor {
    std::string unit_id;
    std::string tenant_id;
    UnitState   state = UnitState::kActive;
    IndexType   index_type = IndexType::kHnsw;
    std::optional<int64_t> sealed_at;       // null until sealed (Phase 2)
    std::optional<int64_t> archived_at;     // null until archived (Phase 2)
    int64_t     vector_count = 0;
    int64_t     size_bytes = 0;
    int64_t     last_write_at = 0;
    int64_t     seal_threshold_vectors = INT64_MAX;  // Phase 1: never seal
    int64_t     seal_threshold_bytes = INT64_MAX;
    int32_t     seal_idle_days = INT32_MAX;
    std::string owner_node_id = "local";    // Phase 2 multi-node
    int64_t     created_at = 0;
};

/// Runtime statistics pushed back to the catalog (UpdateUnitStats).
struct UnitStats {
    int64_t vector_count = 0;
    int64_t size_bytes = 0;
    int64_t last_write_at = 0;
};

/// A namespace's catalog metadata (GetNamespace / namespaces row).
/// The 11 *_config columns are carried as raw JSON strings ('{}' = disabled);
/// ConfigResolver<T> interprets them per consumer — the catalog does not parse.
struct NSMetadata {
    std::string namespace_id;               // namespaces.ns_id
    std::string name;
    std::string tenant_id;
    std::string isolation_mode = "shared";  // 'shared' | 'isolated'
    std::string visibility = "tenant";      // 'tenant' | 'private' | 'public'
    std::optional<std::string> owner_user_id;
    std::optional<std::string> cloned_from_ns_id;
    std::optional<std::string> clone_type;
    std::optional<int64_t>     cloned_at;
    std::string status = "active";          // 'active' | 'deleted'
    int64_t     created_at = 0;

    /// The 11 standardized *_config JSON blobs. Empty optional
    /// means "column default '{}'" — kept as strings; Feature config types live
    /// with each Feature and are resolved by ConfigResolver<T> (Wave 3).
    std::optional<std::string> reranker_config;
    std::optional<std::string> enricher_config;
    std::optional<std::string> parser_config;
    std::optional<std::string> memory_config;
    std::optional<std::string> watcher_config;
    std::optional<std::string> cleaning_config;
    std::optional<std::string> rag_fusion_config;
    std::optional<std::string> crag_config;
    std::optional<std::string> complexity_config;
    std::optional<std::string> sparse_config;
    std::optional<std::string> doc_summary_config;
};

/// Options for INSRouter::ListNamespaces. Phase 1 supports a tenant
/// filter + simple pagination; fields are additive (api_version stable).
struct ListNamespacesOptions {
    std::optional<std::string> tenant_id;   // restrict to one Tenant
    bool include_deleted = false;           // include status='deleted'
    int  limit = 0;                          // 0 = no limit
    int  offset = 0;
};

}  // namespace cortrix::catalog
