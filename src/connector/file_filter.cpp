#include "cortrix/connector/file_filter.h"
#include <algorithm>
#include <filesystem>

namespace cortrix {

FileFilter::FileFilter(const FilterConfig& config) : config_(config) {}

bool FileFilter::ShouldImport(const std::string& path) const {
    namespace fs = std::filesystem;
    std::string filename = fs::path(path).filename().string();

    if (filename.empty()) return false;

    // 1. Hidden file (dot prefix)
    if (filename[0] == '.') return false;

    // 2. Tilde suffix (temp file)
    if (filename.back() == '~') return false;

    // 3. Check ignore patterns
    for (const auto& pattern : config_.ignore_patterns) {
        if (MatchesPattern(filename, pattern)) return false;
    }

    // 4. Check allowed extensions whitelist
    if (!config_.allowed_extensions.empty()) {
        std::string ext = fs::path(path).extension().string();
        // Convert to lowercase for case-insensitive matching
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        bool found = false;
        for (const auto& allowed : config_.allowed_extensions) {
            std::string a = allowed;
            // Ensure dot prefix
            if (!a.empty() && a[0] != '.') a = "." + a;
            std::string a_lower = a;
            std::transform(a_lower.begin(), a_lower.end(), a_lower.begin(), ::tolower);
            if (ext == a_lower) {
                found = true;
                break;
            }
        }
        if (!found) return false;
    }

    return true;
}

bool FileFilter::ShouldTraverse(const std::string& dir_name) const {
    if (dir_name.empty()) return true;

    // Skip hidden directories (dot prefix)
    if (dir_name[0] == '.') return false;

    // Check ignore patterns against directory name
    for (const auto& pattern : config_.ignore_patterns) {
        if (MatchesPattern(dir_name, pattern)) return false;
    }

    return true;
}

bool FileFilter::MatchesPattern(const std::string& filename,
                                 const std::string& pattern) const {
    // Simple glob matching supporting * and ? wildcards
    // Uses recursive approach for simplicity
    size_t fi = 0, pi = 0;
    size_t flen = filename.size(), plen = pattern.size();
    size_t star_pi = std::string::npos, star_fi = 0;

    while (fi < flen) {
        if (pi < plen && (pattern[pi] == '?' || pattern[pi] == filename[fi])) {
            ++fi;
            ++pi;
        } else if (pi < plen && pattern[pi] == '*') {
            star_pi = pi;
            star_fi = fi;
            ++pi;
        } else if (star_pi != std::string::npos) {
            pi = star_pi + 1;
            ++star_fi;
            fi = star_fi;
        } else {
            return false;
        }
    }

    // Consume trailing *'s
    while (pi < plen && pattern[pi] == '*') {
        ++pi;
    }

    return pi == plen;
}

}  // namespace cortrix
