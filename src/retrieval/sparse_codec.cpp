#include <cstdint>
#include "cortrix/retrieval/sparse_codec.h"

#include <cstring>

#include "cortrix/retrieval/sparse_error.h"

namespace cortrix::retrieval {

namespace {
// Append a little-endian POD to a byte buffer (host is LE on all Cortrix targets;
// kept explicit so the on-disk format does not silently depend on host endianness
// the way a raw memcpy of a struct would).
template <typename T>
void AppendLE(std::vector<uint8_t>& out, T value) {
    static_assert(std::is_trivially_copyable_v<T>);
    const auto* p = reinterpret_cast<const uint8_t*>(&value);
    out.insert(out.end(), p, p + sizeof(T));
}

template <typename T>
T ReadLE(const uint8_t* p) {
    T value;
    std::memcpy(&value, p, sizeof(T));
    return value;
}
}  // namespace

size_t SerializedSparseVecSize(const SparseVector& vec) {
    // [uint16 num_terms] + num_terms × ([uint16 term_id][float weight])
    return sizeof(uint16_t) + vec.terms.size() * (sizeof(uint16_t) + sizeof(float));
}

std::vector<uint8_t> SerializeSparseVec(const SparseVector& vec, bool* ok) {
    std::vector<uint8_t> out;
    // num_terms is uint16 (§4.2); a vector with > 65535 active terms cannot be
    // represented — that never happens after top-K (K≤200) but guard anyway.
    if (vec.terms.size() > 0xFFFFu) {
        if (ok) *ok = false;
        return out;
    }
    // term_id is uint16 (§4.2). The standalone stub keeps ids in range; reject an
    // out-of-range id rather than truncate it (a silent truncation would corrupt
    // the inverted index). Real 250K-vocab ids = Phase 2 wider encoding.
    for (const auto& [term_id, weight] : vec.terms) {
        (void)weight;
        if (term_id > 0xFFFFu) {
            if (ok) *ok = false;
            return out;
        }
    }

    out.reserve(SerializedSparseVecSize(vec));
    AppendLE<uint16_t>(out, static_cast<uint16_t>(vec.terms.size()));
    for (const auto& [term_id, weight] : vec.terms) {
        AppendLE<uint16_t>(out, static_cast<uint16_t>(term_id));
        AppendLE<float>(out, weight);
    }
    if (ok) *ok = true;
    return out;
}

Result<SparseVector> DeserializeSparseVec(const uint8_t* data, size_t len) {
    SparseVector vec;
    // Empty buffer (e.g. a NULL column read) → empty vector, not an error.
    if (len == 0 || data == nullptr) {
        return vec;
    }
    if (len < sizeof(uint16_t)) {
        return SparseStatus(SparseErrorCode::kSparseSerializeFailed,
                            "blob too short for num_terms header");
    }
    uint16_t num_terms = ReadLE<uint16_t>(data);
    constexpr size_t kEntrySize = sizeof(uint16_t) + sizeof(float);  // 6
    size_t expected = sizeof(uint16_t) + static_cast<size_t>(num_terms) * kEntrySize;
    if (len < expected) {
        return SparseStatus(
            SparseErrorCode::kSparseSerializeFailed,
            "blob truncated: declared " + std::to_string(num_terms) +
                " terms need " + std::to_string(expected) + " bytes, have " +
                std::to_string(len));
    }
    size_t off = sizeof(uint16_t);
    for (uint16_t i = 0; i < num_terms; ++i) {
        uint16_t term_id = ReadLE<uint16_t>(data + off);
        off += sizeof(uint16_t);
        float weight = ReadLE<float>(data + off);
        off += sizeof(float);
        vec.terms[term_id] = weight;
    }
    return vec;
}

Result<SparseVector> DeserializeSparseVec(const std::vector<uint8_t>& blob) {
    return DeserializeSparseVec(blob.data(), blob.size());
}

}  // namespace cortrix::retrieval
