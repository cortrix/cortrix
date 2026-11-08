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
//   * write sites  -> sanitize and truncate on a character boundary so stored
//     text stays well-formed, and keep the FINAL output inside the documented
//     byte budget;
//   * read sites   -> normalize historical poison (SanitizeUtf8) before the
//     value is re-serialized, so old rows can never take an endpoint down;
//   * untrusted raw bytes (query params, client payloads) -> sanitize before
//     they enter a json that will be dumped.
//
// Budget rule: sanitizing can WIDEN a value (one ill-formed byte becomes a
// 3-byte U+FFFD), so a byte cap applied BEFORE sanitizing does not bound the
// bytes that are actually stored. Every write-side helper below therefore
// applies the cap to the SANITIZED output, and counts any truncation marker
// inside that budget.

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
    if (b0 <= 0xEF) {                                          // 3-byte lead
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
    if (b0 <= 0xF4) {                                          // 4-byte lead
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

// Replace every ill-formed subpart with U+FFFD (one per maximal subpart —
// byte-for-byte what nlohmann's error_handler_t::replace emits, so a
// sanitized value survives later strict dumps untouched). Identity on
// well-formed input.
//
// NOTE: the result can be LONGER than the input (one ill-formed byte becomes a
// 3-byte U+FFFD), so a byte budget must be applied to the SANITIZED output.
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

// Sanitize first, then cap: the returned string is always well-formed UTF-8
// and never longer than max_bytes, even when every input byte was ill-formed
// (sanitizing widens a bad subpart to 3 bytes, so the cap has to be applied
// to the SANITIZED result).
inline std::string Utf8SafeTruncateSanitized(const std::string& s, std::size_t max_bytes) {
    return Utf8SafeTruncate(SanitizeUtf8(s), max_bytes);
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

// Head + mid + tail split that also sanitizes, with a hard byte budget.
//
// The result is always well-formed UTF-8 and never longer than
// head_bytes + mid.size() + tail_bytes:
//   * the head window is the first head_bytes raw bytes, sanitized and then
//     capped back to head_bytes;
//   * the tail window is the last tail_bytes raw bytes, started on a
//     character boundary and then sanitized and capped to tail_bytes.
//
// Sanitizing a window can WIDEN it (one ill-formed byte becomes a 3-byte
// U+FFFD), which is exactly why the cap is applied AFTER the sanitize. A
// boundary-aligned cut on ill-formed input can also collapse the window to
// nothing, so the window is taken raw and cut after the sanitize.
inline std::string Utf8SafeHeadTailSanitized(const std::string& s, std::size_t head_bytes,
                                             const std::string& mid, std::size_t tail_bytes) {
    const std::string head_raw = s.substr(0, head_bytes);
    std::string head = Utf8SafeTruncate(SanitizeUtf8(head_raw), head_bytes);

    std::size_t tail_start = (s.size() > tail_bytes) ? (s.size() - tail_bytes) : 0;
    // Start the tail on a character boundary (walk back off any continuation
    // byte of the character that straddles the window edge).
    while (tail_start > 0 && (static_cast<unsigned char>(s[tail_start]) & 0xC0) == 0x80) {
        --tail_start;
    }
    std::string tail = Utf8SafeTruncate(SanitizeUtf8(s.substr(tail_start)), tail_bytes);
    return head + mid + tail;
}

// Budget-aware head + mid + tail split for telemetry truncation.
//
// The returned string is always well-formed UTF-8 and never longer than
// max_bytes, with the marker counted inside that budget:
//
//   * input already within max_bytes -> the sanitized whole value (no marker);
//   * otherwise -> head_bytes + mid + tail_bytes, where the TAIL shrinks first
//     (the head keeps the documented priority) and the head shrinks only once
//     the tail is exhausted.
//
// The split happens on the SANITIZED bytes, so the windows are measured on the
// bytes that are actually stored.
inline std::string Utf8SafeHeadTailBudgeted(const std::string& s, std::size_t head_bytes,
                                            const std::string& mid, std::size_t tail_bytes,
                                            std::size_t max_bytes) {
    if (s.size() <= max_bytes) {
        // Already within the budget: sanitize and keep the cap (sanitizing can
        // widen a bad subpart to 3 bytes, so the cap is applied to the result).
        return Utf8SafeTruncateSanitized(s, max_bytes);
    }
    std::size_t head_budget = head_bytes;
    std::size_t tail_budget = tail_bytes;
    const std::size_t preferred = head_budget + mid.size() + tail_budget;
    if (preferred > max_bytes) {
        std::size_t excess = preferred - max_bytes;
        const std::size_t tail_cut = (excess < tail_budget) ? excess : tail_budget;
        tail_budget -= tail_cut;
        excess -= tail_cut;
        head_budget = (excess >= head_budget) ? 0 : head_budget - excess;
    }
    std::string out = Utf8SafeHeadTailSanitized(s, head_budget, mid, tail_budget);
    if (out.size() > max_bytes) {
        // Safety net: the budget is the hard invariant, even if a future
        // change to the split arithmetic produced a longer result.
        out = Utf8SafeTruncate(out, max_bytes);
    }
    return out;
}

}  // namespace cortrix::common
