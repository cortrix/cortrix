#pragma once
#include <string>
#include <vector>

namespace cortrix {

struct FilterConfig {
    std::vector<std::string> ignore_patterns;   // glob patterns
    std::vector<std::string> allowed_extensions; // empty = allow all
};

class FileFilter {
public:
    explicit FileFilter(const FilterConfig& config);

    /// Check if a file should be imported
    /// @param path: file absolute path
    /// @return true = should import, false = should skip
    bool ShouldImport(const std::string& path) const;

    /// Check if a directory should be traversed
    /// @param dir_name: directory name (not full path)
    /// @return true = should traverse, false = should skip
    bool ShouldTraverse(const std::string& dir_name) const;

private:
    /// Simplified glob matching (supports * and ? wildcards, . prefix match)
    bool MatchesPattern(const std::string& filename,
                        const std::string& pattern) const;

    FilterConfig config_;
};

}  // namespace cortrix
