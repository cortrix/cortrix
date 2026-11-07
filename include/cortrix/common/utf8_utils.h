#pragma once

// UTF-8 well-formedness + boundary-safe truncation helpers for the JSON
// contract surface.
//
// Why this file exists: nlohmann::json::dump() with its default STRICT error
// handler (used by every WriteJson/WriteJsonResponse call in this repo)
// THROWS json.exception.type_error.316 ("incomplete UTF-8 string; last byte:
// 0x..") when a std::string in the object is not well-formed UTF-8. An
// uncaught 316 from inside a route handler escapes to httplib's global
// set_exception_handler and surfaces as a 500 "CX_ERR_INTERNAL_ERROR /
// Internal server error: [json.exception.type_error.316] ..." (for example on
// GET /api/v1/operations when operation_log.summary was byte-truncated at 100
// bytes in the middle of a multi-byte character).
//
// Close the class at both ends:
//   * write sites  -> truncate on a character boundary (Utf8SafeTruncate /
//     Utf8SafeHeadTail) so stored text stays well-formed;
//   * read sites   -> normalize historical poison (SanitizeUtf8) before the
//     value is re-serialized, so old rows can never take an endpoint down;
//   * untrusted raw bytes (query params, client payloads) -> sanitize before
//     they enter a json that will be dumped.
//
// Validity follows Unicode's Recommended Practice #3 "maximal subpart" policy
// (same reject set and same one-U+FFFD-per-bad-subpart emission as nlohmann's
// error_handler_t::replace): rejects overlong forms, surrogates U+D800..U+DFFF,
// code points > U+10FFFF, stray continuation bytes, and truncated sequences.

#include <cstddef>
#include <string>

namespace cortrix::common {
namespace detail {

// Examine the sequence starting at p (avail bytes left).
// Returns true and sets `used` to the full sequence length when p begins a
// well-formed UTF-8 sequence. Returns false and sets `used` to the length of
// the maximal subpart (>= 1) that could begin a well-formed sequence, so the
// caller can skip exactly the offending bytes and emit one U+FFFD.
inline bool Utf8Seq(const unsigned char* p, size_t avail, size_t& used) {
    const unsigned char b0 = p[0];
    if (b0 < 0x80) {  // ASCII
        used = 1;
        return true;
    }
    if (b0 < 0xC2) {  // 0x80..0xBF stray continuation, 0xC0/0xC1 overlong lead
        used = 1;
        return false;
    }
    if (b0 <= 0xDF) {  // 2-byte lead
        if (avail >= 2 && p[1] >= 0x80 && p[1] <= 0xBF) {
            used = 2;
            return true;
        }
        used = 1;  // truncated or invalid second byte: maximal subpart is the lead
        return false;
    }
    if (b0 <= 0xEF) {  // 3-byte lead
        const unsigned char lo2 = (b0 == 0xE0) ? 0xA0 : 0x80;  // reject overlong
        const unsigned char hi2 = (b0 == 0xED) ? 0x9F : 0xBF;  // reject surrogates
        if (avail < 2 || p[1] < lo2 || p[1] > hi2) {
            used = 1;
            return false;
        }
        if (avail < 3 || p[2] < 0x80 || p[2] > 0xBF) {
            used = 2;
            return false;
        }
        used = 3;
        return true;
    }
    if (b0 <= 0xF4) {  // 4-byte lead
        const unsigned char lo2 = (b0 == 0xF0) ? 0x90 : 0x80;  // reject overlong
        const unsigned char hi2 = (b0 == 0xF4) ? 0x8F : 0xBF;  // reject > U+10FFFF
        if (avail < 2 || p[1] < lo2 || p[1] > hi2) {
            used = 1;
            return false;
        }
        if (avail < 3 || p[2] < 0x80 || p[2] > 0xBF) {
            used = 2;
            return false;
        }
        if (avail < 4 || p[3] < 0x80 || p[3] > 0xBF) {
            used = 3;
            return false;
        }
        used = 4;
        return true;
    }
    used = 1;  // 0xF5..0xFF: never valid UTF-8
    return false;
}

}  // namespace detail

// True iff s is well-formed UTF-8 (the exact accept set of nlohmann's strict
// serializer, minus control-character escaping which is representational).
inline bool IsWellFormedUtf8(const std::string& s) {
    const unsigned char* p = reinterpret_cast<const unsigned char*>(s.data());
    size_t i = 0;
    while (i < s.size()) {
        size_t used = 0;
        if (!detail::Utf8Seq(p + i, s.size() - i, used)) return false;
        i += used;
    }
    return true;
}

// Longest prefix of s that is at most max_bytes AND does not cut a multi-byte
// character (walks back to the lead byte of any sequence straddling the cut).
// Guarantees only that the CUT is clean: if s already contains ill-formed
// bytes the prefix can still hold them — SanitizeUtf8 those inputs separately.
inline std::string Utf8SafeTruncate(const std::string& s, std::size_t max_bytes) {
    if (s.size() <= max_bytes) return s;
    std::size_t pos = max_bytes;
    while (pos > 0 && (static_cast<unsigned char>(s[pos]) & 0xC0) == 0x80) --pos;
    return s.substr(0, pos);
}

// Head + mid + tail split for telemetry truncation (§3-style "first N + ... +
// last M bytes"): both sides are character-boundary aligned (the tail starts
// at the first non-continuation byte at or after s.size() - tail_bytes).
// Like Utf8SafeTruncate, this aligns the CUTS only; sanitize the result for
// untrusted inputs.
inline std::string Utf8SafeHeadTail(const std::string& s, std::size_t head_bytes,
                                    const std::string& mid, std::size_t tail_bytes) {
    const std::string head = Utf8SafeTruncate(s, head_bytes);
    std::size_t tstart = (s.size() > tail_bytes) ? (s.size() - tail_bytes) : 0;
    // Walk forward off any continuation bytes so the tail starts on a
    // lead byte (or on nothing at all if the string is entirely continuations).
    while (tstart < s.size() && (static_cast<unsigned char>(s[tstart]) & 0xC0) == 0x80) {
        ++tstart;
    }
    return head + mid + s.substr(tstart);
}

// Replace every ill-formed subpart with U+FFFD (one per maximal subpart —
// byte-for-byte what nlohmann's error_handler_t::replace emits, so a
// sanitized value survives later strict dumps untouched). Identity on
// well-formed input.
inline std::string SanitizeUtf8(const std::string& s) {
    if (IsWellFormedUtf8(s)) return s;  // fast path keeps hot readers allocation-free
    std::string out;
    out.reserve(s.size());
    const unsigned char* p = reinterpret_cast<const unsigned char*>(s.data());
    size_t i = 0;
    while (i < s.size()) {
        size_t used = 0;
        if (detail::Utf8Seq(p + i, s.size() - i, used)) {
            out.append(s, i, used);
        } else {
            out += "\xEF\xBF\xBD";  // U+FFFD
        }
        i += used;
    }
    return out;
}

}  // namespace cortrix::common
