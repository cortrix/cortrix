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

}  // namespace cortrix::common
