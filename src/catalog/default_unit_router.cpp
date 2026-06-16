#include <cstdint>
#include "cortrix/catalog/default_unit_router.h"

#include <sqlite3.h>

#include <string>

#include "cortrix/catalog/catalog_error.h"

namespace cortrix::catalog {

namespace {

std::string ColText(sqlite3_stmt* stmt, int col) {
    const char* t = reinterpret_cast<const char*>(sqlite3_column_text(stmt, col));
    return t ? std::string(t) : std::string();
}

std::optional<int64_t> ColIntOpt(sqlite3_stmt* stmt, int col) {
    if (sqlite3_column_type(stmt, col) == SQLITE_NULL) return std::nullopt;
    return static_cast<int64_t>(sqlite3_column_int64(stmt, col));
}

}  // namespace

DefaultUnitRouter::DefaultUnitRouter(sqlite3* db) : db_(db) {}

Result<UnitDescriptor> DefaultUnitRouter::ReadUnitLocked(
    const std::string& unit_id) const {
    const char* sql =
        "SELECT unit_id, tenant_id, state, index_type, sealed_at, archived_at, "
        "vector_count, size_bytes, last_write_at, seal_threshold_vectors, "
        "seal_threshold_bytes, seal_idle_days, owner_node_id, created_at "
        "FROM units WHERE unit_id = ?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return CatalogStatus(CatalogErrorCode::kInternalError,
                             std::string("prepare GetUnit: ") + sqlite3_errmsg(db_));
    }
    sqlite3_bind_text(stmt, 1, unit_id.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return CatalogStatus(CatalogErrorCode::kUnitNotFound, "unit '" + unit_id + "'");
    }
    UnitDescriptor u;
    u.unit_id       = ColText(stmt, 0);
    u.tenant_id     = ColText(stmt, 1);
    u.state         = UnitStateFromString(ColText(stmt, 2));
    u.index_type    = IndexTypeFromString(ColText(stmt, 3));
    u.sealed_at     = ColIntOpt(stmt, 4);
    u.archived_at   = ColIntOpt(stmt, 5);
    u.vector_count  = sqlite3_column_int64(stmt, 6);
    u.size_bytes    = sqlite3_column_int64(stmt, 7);
    u.last_write_at = sqlite3_column_int64(stmt, 8);
    u.seal_threshold_vectors = sqlite3_column_int64(stmt, 9);
    u.seal_threshold_bytes   = sqlite3_column_int64(stmt, 10);
    u.seal_idle_days = static_cast<int32_t>(sqlite3_column_int(stmt, 11));
    u.owner_node_id = ColText(stmt, 12);
    u.created_at    = sqlite3_column_int64(stmt, 13);
    sqlite3_finalize(stmt);
    return u;
}

Result<UnitDescriptor> DefaultUnitRouter::GetUnit(const std::string& unit_id) const {
    std::lock_guard<std::mutex> lock(mu_);
    return ReadUnitLocked(unit_id);
}

Result<UnitState> DefaultUnitRouter::GetUnitState(const std::string& unit_id) const {
    std::lock_guard<std::mutex> lock(mu_);
    Result<UnitDescriptor> u = ReadUnitLocked(unit_id);
    if (!u.ok()) return u.status();
    return u.value().state;
}

Status DefaultUnitRouter::UpdateUnitState(const std::string& unit_id,
                                          UnitState new_state) {
    std::lock_guard<std::mutex> lock(mu_);
    Result<UnitDescriptor> u = ReadUnitLocked(unit_id);
    if (!u.ok()) return u.status();

    // Phase 1: only the identity transition active→active is meaningful. Any
    // real transition belongs to the Phase 2 state machine.
    if (!(u.value().state == UnitState::kActive && new_state == UnitState::kActive)) {
        return CatalogStatus(CatalogErrorCode::kNotImplemented,
                             std::string("Unit state machine is Phase 2 (requested ") +
                                 ToString(new_state) + ")");
    }
    return Status::Ok();  // no-op write avoided; state already active
}

Status DefaultUnitRouter::UpdateUnitStats(const std::string& unit_id,
                                          const UnitStats& stats) {
    std::lock_guard<std::mutex> lock(mu_);
    const char* sql =
        "UPDATE units SET vector_count = ?, size_bytes = ?, last_write_at = ? "
        "WHERE unit_id = ?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return CatalogStatus(CatalogErrorCode::kInternalError,
                             std::string("prepare UpdateUnitStats: ") + sqlite3_errmsg(db_));
    }
    sqlite3_bind_int64(stmt, 1, stats.vector_count);
    sqlite3_bind_int64(stmt, 2, stats.size_bytes);
    sqlite3_bind_int64(stmt, 3, stats.last_write_at);
    sqlite3_bind_text(stmt, 4, unit_id.c_str(), -1, SQLITE_TRANSIENT);
    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        return CatalogStatus(CatalogErrorCode::kInternalError, "UpdateUnitStats failed");
    }
    if (sqlite3_changes(db_) == 0) {
        return CatalogStatus(CatalogErrorCode::kUnitNotFound, "unit '" + unit_id + "'");
    }
    return Status::Ok();
}

Result<bool> DefaultUnitRouter::ShouldSeal(const std::string& unit_id) const {
    std::lock_guard<std::mutex> lock(mu_);
    Result<UnitDescriptor> u = ReadUnitLocked(unit_id);
    if (!u.ok()) return u.status();
    // Phase 1: thresholds default to INT_MAX so this never trips. Phase 2
    // evaluates the OR of the three seal conditions (vectors / bytes / idle).
    const UnitDescriptor& d = u.value();
    const bool by_vectors = d.vector_count >= d.seal_threshold_vectors;
    const bool by_bytes   = d.size_bytes   >= d.seal_threshold_bytes;
    return by_vectors || by_bytes;  // idle-days check is Phase 2 (needs clock)
}

}  // namespace cortrix::catalog
