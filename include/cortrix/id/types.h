#pragma once
#include <cstdint>
#include <string>
// SoT: ARCHITECTURE.md § 1.8 global ID system (D1 V3 ruling 3). This header = the code realization of the §1.8.1 ID type definitions
// (missed by the Sprint 0 scaffolding, backfilled later).
// Note: currently a type alias (=std::string), providing no compile-time mix-up protection; distinct strong types = Phase 2 (see PHASE2_BACKLOG TD-ID-DISTINCT-STRONGTYPE).
namespace cortrix::id {
using ChildId     = std::string;   // ULID, 26 chars, time-ordered
using ParentId    = std::string;   // ULID, 26 chars
using DocId       = std::string;   // ULID, 26 chars
using BlockId     = uint64_t;      // 64 bit, P-HNSW internal
using NamespaceId = std::string;   // user-defined string

// BlockId (uint64) <-> SQLite INTEGER (signed int64). SQLite has no unsigned 64-bit
// type, so a BlockId with the high bit set (SipHash-2-4 output frequently has it) is
// stored as the bit-identical *negative* int64 and read back identically. C++17 has
// no std::bit_cast, but static_cast between uint64_t and int64_t is bit-preserving on
// two's-complement platforms (all Cortrix targets: x86-64 / arm64). Centralized here
// so no raw casts leak — D3.5 store wiring uses these at every block_id bind/read.
inline int64_t ToSqliteInt(BlockId id) {
    static_assert(sizeof(BlockId) == sizeof(int64_t) && sizeof(BlockId) == 8,
                  "BlockId must be exactly 64-bit for a bit-preserving SQLite round-trip");
    return static_cast<int64_t>(id);
}
inline BlockId FromSqliteInt(int64_t v) { return static_cast<BlockId>(v); }
}  // namespace cortrix::id
