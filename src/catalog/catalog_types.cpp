#include "cortrix/catalog/catalog_types.h"

#include <string>

namespace cortrix::catalog {

const char* ToString(UnitState state) {
    switch (state) {
        case UnitState::kActive:    return "active";
        case UnitState::kSealing:   return "sealing";
        case UnitState::kSealed:    return "sealed";
        case UnitState::kArchived:  return "archived";
        case UnitState::kMigrating: return "migrating";
    }
    return "active";  // unreachable for a valid enum; conservative default
}

UnitState UnitStateFromString(const std::string& s) {
    if (s == "sealing")   return UnitState::kSealing;
    if (s == "sealed")    return UnitState::kSealed;
    if (s == "archived")  return UnitState::kArchived;
    if (s == "migrating") return UnitState::kMigrating;
    return UnitState::kActive;  // default incl. unknown (Phase 1 conservative)
}

const char* ToString(IndexType type) {
    switch (type) {
        case IndexType::kHnsw:          return "hnsw";
        case IndexType::kBruteForce:    return "bruteforce";
        case IndexType::kQuantizedHnsw: return "quantized_hnsw";
    }
    return "hnsw";  // unreachable for a valid enum; conservative default
}

IndexType IndexTypeFromString(const std::string& s) {
    if (s == "bruteforce")     return IndexType::kBruteForce;
    if (s == "quantized_hnsw") return IndexType::kQuantizedHnsw;
    return IndexType::kHnsw;  // default incl. unknown
}

}  // namespace cortrix::catalog
