#pragma once
#include <cstdint>
#include <string>

namespace cortrix::query {

/// content_hash representation helpers for cross-NS dedup (RETRIEVAL_TYPES_SPEC §6
/// + F04 §3.3.2). The response/dedup layer uses the string form
/// `"sha256:<32-hex>"` (the first 16 bytes of SHA-256, hex-encoded) as the dedup
/// hash-table key; the storage layer keeps `uint64×2` raw.
///
/// 🚨 D3 standalone / F09-4 hook: in production ResultItem.content_hash is sourced
/// from the F09 Block header (uint64×2) via ContentHashToString. F08/F09 wiring is
/// **D3.5**. Standalone, SingleUnitExecutor derives the hash directly from the
/// chunk content with ContentHashOfContent so dedup is exercisable end-to-end
/// against the same `"sha256:..."` representation the production path will emit.

/// 16-byte raw (hi, lo) → "sha256:<32-hex>" (RETRIEVAL_TYPES_SPEC §6,
/// ContentHashToString). Big-endian hex of `hi` then `lo` (16 bytes → 32 hex).
std::string ContentHashToString(uint64_t hi, uint64_t lo);

/// "sha256:<32-hex>" → 16-byte raw (hi, lo). Returns false on a malformed string
/// (wrong prefix / length / non-hex); *hi/*lo are left unspecified on failure.
bool ContentHashFromString(const std::string& s, uint64_t* hi, uint64_t* lo);

/// SHA-256 of `content`, first 16 bytes, as "sha256:<32-hex>". Deterministic and
/// content-addressed so two NS holding byte-identical chunk text collapse to one
/// dedup key (§3.3 B simplified). Standalone substitute for the F09 header hash.
std::string ContentHashOfContent(const std::string& content);

}  // namespace cortrix::query
