#pragma once

#include <string>
#include <string_view>

namespace cortrix::common {

inline bool IsAsciiWhitespace(char c) {
    return c == ' ' || c == '\n' || c == '\r' || c == '\t' || c == '\f' || c == '\v';
}

inline std::string_view TrimAsciiWhitespace(std::string_view s) {
    while (!s.empty() && IsAsciiWhitespace(s.front())) {
        s.remove_prefix(1);
    }
    while (!s.empty() && IsAsciiWhitespace(s.back())) {
        s.remove_suffix(1);
    }
    return s;
}

// Extract the first balanced top-level JSON object from `input`: from the
// first '{' to its matching '}', honoring strings and escapes so braces inside
// string values do not miscount. Returns "" when no balanced object exists
// (no '{', or the object never closes — e.g. a truncated generation).
//
// This is the SECOND-CHANCE repair layer for LLM JSON contracts (deep-QA
// 2026-07-10, DeepSeek-V4-Flash x F41 field failure): providers occasionally
// wrap the object in prose ("Here is the JSON: {...}") or an UNCLOSED fence,
// which the strict path below rightly refuses. Callers must treat a repair as
// an observable event (log/diagnostics), never as the silent normal path.
inline std::string ExtractFirstBalancedJsonObject(const std::string& input) {
    const size_t start = input.find('{');
    if (start == std::string::npos) return "";
    int depth = 0;
    bool in_string = false;
    bool escaped = false;
    for (size_t i = start; i < input.size(); ++i) {
        const char c = input[i];
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }
        if (c == '"') {
            in_string = true;
        } else if (c == '{') {
            ++depth;
        } else if (c == '}') {
            --depth;
            if (depth == 0) return input.substr(start, i - start + 1);
        }
    }
    return "";  // unbalanced (e.g. truncated) — nothing safe to repair
}

// Accept only a complete Markdown JSON code fence:
//   ```json
//   {...}
//   ```
// This intentionally does not recover JSON from surrounding prose or partial text.
inline std::string UnwrapCompleteJsonFence(const std::string& input) {
    std::string_view trimmed = TrimAsciiWhitespace(input);
    if (trimmed.size() < 6 || trimmed.substr(0, 3) != "```") {
        return input;
    }

    size_t first_line_end = trimmed.find('\n');
    if (first_line_end == std::string_view::npos) {
        return std::string(trimmed);
    }

    std::string_view opener = trimmed.substr(0, first_line_end);
    if (!opener.empty() && opener.back() == '\r') {
        opener.remove_suffix(1);
    }
    std::string_view language = TrimAsciiWhitespace(opener.substr(3));
    if (language.find('`') != std::string_view::npos) {
        return std::string(trimmed);
    }

    size_t closing = trimmed.rfind("```");
    if (closing == std::string_view::npos || closing == 0 ||
        closing + 3 != trimmed.size()) {
        return std::string(trimmed);
    }

    std::string_view inner =
        trimmed.substr(first_line_end + 1, closing - first_line_end - 1);
    if (!inner.empty() && inner.back() == '\n') {
        inner.remove_suffix(1);
    }
    if (!inner.empty() && inner.back() == '\r') {
        inner.remove_suffix(1);
    }
    return std::string(inner);
}

// JSON string-escape repair (QA 2026-07-12 F-10; first shipped F41-only after
// the 2026-07-11 DeepSeek field failure): providers sometimes emit an invalid
// backslash escape inside a string value (`C:\Users`, LaTeX `\_`, kaomoji) —
// strict parsing rightly refuses, and it refuses DETERMINISTICALLY for the
// same input text, so retries cannot help. Doubling the offending backslash
// preserves the author-visible text and lets the object parse; valid escapes
// pass through untouched, so repairing an already-valid document is a no-op.
// Same discipline as the balanced-object layer above: strict parse first,
// repair only after it fails, keep the event observable.
inline bool IsValidJsonEscape(char c) {
    switch (c) {
        case '"':
        case '\\':
        case '/':
        case 'b':
        case 'f':
        case 'n':
        case 'r':
        case 't':
        case 'u':
            return true;
        default:
            return false;
    }
}

inline std::string EscapeInvalidJsonStringBackslashes(const std::string& value) {
    std::string out;
    out.reserve(value.size());

    bool in_string = false;
    bool after_backslash = false;
    for (char c : value) {
        if (!in_string) {
            if (c == '"') in_string = true;
            out.push_back(c);
            continue;
        }

        if (after_backslash) {
            if (!IsValidJsonEscape(c)) out.push_back('\\');
            out.push_back(c);
            after_backslash = false;
            continue;
        }

        if (c == '\\') {
            out.push_back(c);
            after_backslash = true;
            continue;
        }

        if (c == '"') in_string = false;
        out.push_back(c);
    }

    if (after_backslash) out.push_back('\\');
    return out;
}

}  // namespace cortrix::common
