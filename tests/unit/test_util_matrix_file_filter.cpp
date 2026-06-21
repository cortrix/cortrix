// Exhaustive deterministic matrices for connector/file_filter.h (FileFilter).
// Covers ShouldImport (dot-hidden / tilde-suffix / ignore-glob / extension
// whitelist) and ShouldTraverse, plus the simplified glob engine exercised
// indirectly through ignore_patterns.
//
// Decision rules from src/connector/file_filter.cpp:
//  - empty filename -> not imported
//  - leading '.' -> not imported (hidden)
//  - trailing '~' -> not imported (temp)
//  - any ignore glob match -> not imported
//  - non-empty allowed_extensions whitelist -> extension (lowercased, dot form)
//    must be in the whitelist, else not imported
//  - ShouldTraverse: empty -> true; leading '.' -> false; ignore glob -> false.
//  - glob: '*' = any run, '?' = single char, exact otherwise; full-string match.
//
// SUITE PREFIX: FileFilterUtilMatrix* (unique; FileFilterTest already exists).
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "cortrix/connector/file_filter.h"

namespace cortrix {
namespace {

FilterConfig MakeConfig(std::vector<std::string> ignore,
                        std::vector<std::string> exts = {}) {
    FilterConfig c;
    c.ignore_patterns = std::move(ignore);
    c.allowed_extensions = std::move(exts);
    return c;
}

// ---- ShouldImport with the canonical ignore set, no extension whitelist ----
struct ImportCase {
    std::string path;
    bool expected;  // true = should import
};

class FileFilterUtilMatrixImport : public ::testing::TestWithParam<ImportCase> {
protected:
    void SetUp() override {
        filter_ = std::make_unique<FileFilter>(
            MakeConfig({".*", "*~", "*.tmp", "*.swp", "#*#", "*.bak"}));
    }
    std::unique_ptr<FileFilter> filter_;
};

TEST_P(FileFilterUtilMatrixImport, Decision) {
    const ImportCase& c = GetParam();
    EXPECT_EQ(filter_->ShouldImport(c.path), c.expected) << "path=" << c.path;
}

INSTANTIATE_TEST_SUITE_P(
    Cases, FileFilterUtilMatrixImport,
    ::testing::Values(
        // Normal files -> import.
        ImportCase{"/home/user/file.txt", true},
        ImportCase{"/a/b/report.pdf", true},
        ImportCase{"plain", true},
        ImportCase{"/x/data.json", true},
        ImportCase{"/x/code.cpp", true},
        ImportCase{"/x/archive.tar.gz", true},
        // Hidden dotfiles -> skip.
        ImportCase{"/home/user/.gitignore", false},
        ImportCase{"/home/user/.DS_Store", false},
        ImportCase{"/home/.bashrc", false},
        ImportCase{".env", false},
        ImportCase{"/d/.hidden", false},
        // Tilde temp suffix -> skip.
        ImportCase{"/home/user/document~", false},
        ImportCase{"draft~", false},
        ImportCase{"/x/notes.txt~", false},
        // Ignore-glob matches -> skip.
        ImportCase{"/x/scratch.tmp", false},
        ImportCase{"/x/.file.swp", false},  // also hidden, but glob covers .swp form
        ImportCase{"/x/file.swp", false},
        ImportCase{"/x/old.bak", false},
        ImportCase{"#emacs#", false},
        ImportCase{"#autosave#", false},
        // Non-matching glob neighbours -> import.
        ImportCase{"/x/file.tmpx", true},   // .tmp != .tmpx
        ImportCase{"/x/file.swpa", true},
        ImportCase{"/x/backup", true},      // not *.bak
        ImportCase{"/x/file.tmp.txt", true},
        // Empty -> skip (empty filename).
        ImportCase{"", false},
        // Trailing slash means empty filename component -> skip.
        ImportCase{"/x/dir/", false}));

// ---- ShouldImport with an extension whitelist ----
struct WhitelistCase {
    std::string path;
    bool expected;
};

class FileFilterUtilMatrixWhitelist
    : public ::testing::TestWithParam<WhitelistCase> {
protected:
    void SetUp() override {
        // Mixed dot/no-dot, mixed case whitelist entries.
        filter_ = std::make_unique<FileFilter>(
            MakeConfig({}, {"txt", ".pdf", "MD", ".JSON"}));
    }
    std::unique_ptr<FileFilter> filter_;
};

TEST_P(FileFilterUtilMatrixWhitelist, Decision) {
    const WhitelistCase& c = GetParam();
    EXPECT_EQ(filter_->ShouldImport(c.path), c.expected) << "path=" << c.path;
}

INSTANTIATE_TEST_SUITE_P(
    Cases, FileFilterUtilMatrixWhitelist,
    ::testing::Values(
        // Allowed extensions (case-insensitive both sides).
        WhitelistCase{"/x/a.txt", true},
        WhitelistCase{"/x/A.TXT", true},
        WhitelistCase{"/x/b.pdf", true},
        WhitelistCase{"/x/B.PDF", true},
        WhitelistCase{"/x/c.md", true},
        WhitelistCase{"/x/C.MD", true},
        WhitelistCase{"/x/d.json", true},
        WhitelistCase{"/x/D.JSON", true},
        // Multi-dot: final extension is what matters.
        WhitelistCase{"/x/archive.tar.txt", true},
        WhitelistCase{"/x/v1.2.pdf", true},
        // Disallowed extensions -> skip.
        WhitelistCase{"/x/e.csv", false},
        WhitelistCase{"/x/f.png", false},
        WhitelistCase{"/x/g.docx", false},
        WhitelistCase{"/x/noext", false},
        WhitelistCase{"/x/h.tar.gz", false},
        // Hidden still wins over whitelist (checked before extension).
        WhitelistCase{"/x/.config.txt", false}));

// ---- ShouldTraverse ----
struct TraverseCase {
    std::string dir;
    bool expected;
};

class FileFilterUtilMatrixTraverse
    : public ::testing::TestWithParam<TraverseCase> {
protected:
    void SetUp() override {
        filter_ = std::make_unique<FileFilter>(
            MakeConfig({"node_modules", "*.cache", "build?"}));
    }
    std::unique_ptr<FileFilter> filter_;
};

TEST_P(FileFilterUtilMatrixTraverse, Decision) {
    const TraverseCase& c = GetParam();
    EXPECT_EQ(filter_->ShouldTraverse(c.dir), c.expected) << "dir=" << c.dir;
}

INSTANTIATE_TEST_SUITE_P(
    Cases, FileFilterUtilMatrixTraverse,
    ::testing::Values(
        // Empty -> traverse (true).
        TraverseCase{"", true},
        // Normal dirs -> traverse.
        TraverseCase{"src", true},
        TraverseCase{"docs", true},
        TraverseCase{"my_project", true},
        // Hidden dirs -> skip.
        TraverseCase{".git", false},
        TraverseCase{".cache", false},
        TraverseCase{".idea", false},
        // Exact ignore match.
        TraverseCase{"node_modules", false},
        // Glob ignore: *.cache.
        TraverseCase{"foo.cache", false},
        TraverseCase{"x.cache", false},
        TraverseCase{".cache", false},  // hidden first, also matches glob
        // Glob ignore: build? (single trailing char).
        TraverseCase{"builds", false},
        TraverseCase{"buildX", false},
        TraverseCase{"build", true},     // no char to match '?'
        TraverseCase{"buildXY", true},   // two extra chars, '?' = exactly one
        // Non-matching neighbours.
        TraverseCase{"cache", true},
        TraverseCase{"node_module", true}));

// ---- Glob semantics via ignore_patterns (exercises MatchesPattern) ----
struct GlobCase {
    std::string pattern;
    std::string name;
    bool match;  // true = pattern matches name (=> NOT imported)
};

class FileFilterUtilMatrixGlob : public ::testing::TestWithParam<GlobCase> {};

TEST_P(FileFilterUtilMatrixGlob, ImportReflectsGlobMatch) {
    const GlobCase& c = GetParam();
    // Build a filter whose ONLY rule is this single glob; use a name that is not
    // hidden and has no tilde so the glob is the only deciding factor.
    FileFilter f(MakeConfig({c.pattern}));
    // A glob match => ShouldImport == false.
    EXPECT_EQ(f.ShouldImport("/dir/" + c.name), !c.match)
        << "pattern=" << c.pattern << " name=" << c.name;
}

INSTANTIATE_TEST_SUITE_P(
    Cases, FileFilterUtilMatrixGlob,
    ::testing::Values(
        // Exact literals.
        GlobCase{"exact.txt", "exact.txt", true},
        GlobCase{"exact.txt", "exact.txtx", false},
        GlobCase{"exact.txt", "exact.tx", false},
        // Leading star.
        GlobCase{"*.log", "app.log", true},
        GlobCase{"*.log", "log", false},
        GlobCase{"*.log", "a.b.log", true},
        GlobCase{"*.log", "app.logx", false},
        // Trailing star.
        GlobCase{"tmp*", "tmp", true},
        GlobCase{"tmp*", "tmpfile", true},
        GlobCase{"tmp*", "atmp", false},
        // Star in the middle.
        GlobCase{"a*z", "az", true},
        GlobCase{"a*z", "abcz", true},
        GlobCase{"a*z", "abc", false},
        GlobCase{"a*z", "za", false},
        // Multiple stars.
        GlobCase{"*foo*", "foo", true},
        GlobCase{"*foo*", "xfooy", true},
        GlobCase{"*foo*", "fo", false},
        // Question mark = exactly one char.
        GlobCase{"file.?xt", "file.txt", true},
        GlobCase{"file.?xt", "file.xt", false},
        GlobCase{"a?c", "abc", true},
        GlobCase{"a?c", "ac", false},
        GlobCase{"a?c", "abbc", false},
        // Combined ? and *.
        GlobCase{"?*.dat", "a.dat", true},
        GlobCase{"?*.dat", "ab.dat", true},
        GlobCase{"v?.*", "v1.0", true},
        GlobCase{"v?.*", "v10.0", false},
        // Bare star matches anything.
        GlobCase{"*", "anything", true},
        GlobCase{"*", "x.y.z", true},
        // Empty pattern only matches empty name (so a real name never matches).
        GlobCase{"", "name", false}));

}  // namespace
}  // namespace cortrix
