#pragma once
#include <cstdint>
#include <string>

namespace cortrix {

/// Search route bitmask
enum RouteFlag : uint8_t {
    kRouteVector = 0x01,
    kRouteBM25   = 0x02,
    kRouteSQL    = 0x04,
    kRouteDocId  = 0x08,   // doc_id pre-filter path (bypasses HNSW/BM25)
};

struct ScoredBlock {
    uint64_t  block_id = 0;
    std::string doc_id;
    int      chunk_index = 0;
    float    rrf_score = 0.0f;
    uint8_t  hit_routes = 0;        // RouteFlag bitmask
    float    vector_score = 0.0f;   // raw score from vector route
    float    bm25_score = 0.0f;     // raw score from BM25 route

    /// Number of routes hit (single CPU instruction via popcount).
    /// Design spec (ARCHITECTURE_DEEP_DESIGN.md Section 3.5.3):
    /// "hit_routes bitmask: popcount computes related_blocks_count (a single CPU instruction)"
    int hit_count() const {
        return __builtin_popcount(static_cast<unsigned int>(hit_routes));
    }

    /// Sort by rrf_score descending
    bool operator>(const ScoredBlock& other) const {
        return rrf_score > other.rrf_score;
    }
};

}  // namespace cortrix
