#pragma once
#include <cstdint>
#include <string>

// ULID generation (ARCH § 1.8.1 — IDs are 26-char Crockford-base32 ULIDs:
// 48-bit millisecond timestamp + 80-bit randomness, lexicographically sortable so
// IDs are time-ordered). Scaffolding note: the id/ header previously only carried
// the type aliases (id/types.h); F34 is the first feature to mint IDs, so it adds
// the generator here as the shared home for all id minting (parent/child/doc).
namespace cortrix::id {

/// Generate a fresh 26-char ULID for `now_ms` (Unix epoch milliseconds). Within
/// the same millisecond, successive calls are strictly increasing (the random
/// component is incremented, per the ULID monotonic-factory rule), so a batch
/// minted in one tick still sorts in mint order. Thread-safe.
std::string GenerateUlid(int64_t now_ms);

/// Generate a fresh ULID using the current wall clock.
std::string GenerateUlid();

}  // namespace cortrix::id
