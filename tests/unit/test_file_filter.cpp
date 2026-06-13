#include <gtest/gtest.h>
#include "cortrix/connector/file_filter.h"

namespace cortrix {
namespace {

class FileFilterTest : public ::testing::Test {
protected:
    void SetUp() override {
        FilterConfig config;
        config.ignore_patterns = {".*", "*~", "*.tmp", "*.swp", "#*#"};
        filter_ = std::make_unique<FileFilter>(config);
    }
    std::unique_ptr<FileFilter> filter_;
};

// --- ShouldImport tests ---

TEST_F(FileFilterTest, HiddenFileDot_Gitignore) {
    EXPECT_FALSE(filter_->ShouldImport("/home/user/docs/.gitignore"));
}

TEST_F(FileFilterTest, HiddenFileDot_DSStore) {
    EXPECT_FALSE(filter_->ShouldImport("/home/user/docs/.DS_Store"));
}

TEST_F(FileFilterTest, HiddenFilePrefix) {
    EXPECT_FALSE(filter_->ShouldImport("/home/user/.hidden_config"));
}

TEST_F(FileFilterTest, TempFileTilde) {
    EXPECT_FALSE(filter_->ShouldImport("/home/user/document~"));
}

TEST_F(FileFilterTest, TempFileTmp) {
    EXPECT_FALSE(filter_->ShouldImport("/home/user/data.tmp"));
}

TEST_F(FileFilterTest, SwapFile) {
    EXPECT_FALSE(filter_->ShouldImport("/home/user/file.swp"));
}

TEST_F(FileFilterTest, EmacsAutoSave) {
    EXPECT_FALSE(filter_->ShouldImport("/home/user/#file#"));
}

TEST_F(FileFilterTest, NormalFilePdf) {
    EXPECT_TRUE(filter_->ShouldImport("/home/user/report.pdf"));
}

TEST_F(FileFilterTest, NormalFileMd) {
    EXPECT_TRUE(filter_->ShouldImport("/home/user/notes.md"));
}

TEST_F(FileFilterTest, NormalFilePng) {
    EXPECT_TRUE(filter_->ShouldImport("/home/user/image.png"));
}

TEST_F(FileFilterTest, NormalFileTxt) {
    EXPECT_TRUE(filter_->ShouldImport("/home/user/readme.txt"));
}

// --- ShouldTraverse tests ---

TEST_F(FileFilterTest, DirectoryFilter_Git) {
    EXPECT_FALSE(filter_->ShouldTraverse(".git"));
}

TEST_F(FileFilterTest, DirectoryFilter_Hidden) {
    EXPECT_FALSE(filter_->ShouldTraverse(".config"));
}

TEST_F(FileFilterTest, NormalDirectory) {
    EXPECT_TRUE(filter_->ShouldTraverse("subdir"));
}

TEST_F(FileFilterTest, NormalDirectory_Documents) {
    EXPECT_TRUE(filter_->ShouldTraverse("documents"));
}

TEST_F(FileFilterTest, DirectoryFilter_NodeModules) {
    // node_modules doesn't match any default pattern, so it should traverse
    EXPECT_TRUE(filter_->ShouldTraverse("node_modules"));
}

// --- AllowedExtensions tests ---

TEST(FileFilterAllowedExtTest, WhitelistFilters) {
    FilterConfig config;
    config.ignore_patterns = {".*", "*~"};
    config.allowed_extensions = {".pdf", ".md"};
    FileFilter filter(config);

    EXPECT_TRUE(filter.ShouldImport("/home/user/report.pdf"));
    EXPECT_TRUE(filter.ShouldImport("/home/user/notes.md"));
    EXPECT_FALSE(filter.ShouldImport("/home/user/data.txt"));
    EXPECT_FALSE(filter.ShouldImport("/home/user/image.png"));
}

TEST(FileFilterAllowedExtTest, EmptyWhitelistAllowsAll) {
    FilterConfig config;
    config.ignore_patterns = {".*"};
    config.allowed_extensions = {};  // empty = allow all
    FileFilter filter(config);

    EXPECT_TRUE(filter.ShouldImport("/home/user/report.pdf"));
    EXPECT_TRUE(filter.ShouldImport("/home/user/data.txt"));
    EXPECT_TRUE(filter.ShouldImport("/home/user/image.png"));
}

TEST(FileFilterAllowedExtTest, CaseInsensitiveExtension) {
    FilterConfig config;
    config.allowed_extensions = {".pdf"};
    FileFilter filter(config);

    EXPECT_TRUE(filter.ShouldImport("/home/user/report.PDF"));
    EXPECT_TRUE(filter.ShouldImport("/home/user/report.Pdf"));
}

TEST(FileFilterAllowedExtTest, ExtensionWithoutDot) {
    FilterConfig config;
    config.allowed_extensions = {"pdf", "md"};
    FileFilter filter(config);

    EXPECT_TRUE(filter.ShouldImport("/home/user/report.pdf"));
    EXPECT_TRUE(filter.ShouldImport("/home/user/notes.md"));
    EXPECT_FALSE(filter.ShouldImport("/home/user/data.txt"));
}

// --- Edge cases ---

TEST_F(FileFilterTest, EmptyFilename) {
    EXPECT_FALSE(filter_->ShouldImport(""));
}

TEST_F(FileFilterTest, EmptyDirName) {
    EXPECT_TRUE(filter_->ShouldTraverse(""));
}

// --- Glob pattern matching edge cases ---

TEST_F(FileFilterTest, WildcardStar_MatchesAnything) {
    FilterConfig config;
    config.ignore_patterns = {"*.log"};
    FileFilter filter(config);
    EXPECT_FALSE(filter.ShouldImport("/path/to/app.log"));
    EXPECT_TRUE(filter.ShouldImport("/path/to/app.txt"));
}

TEST_F(FileFilterTest, WildcardQuestion_MatchesSingleChar) {
    FilterConfig config;
    config.ignore_patterns = {"file?.txt"};
    FileFilter filter(config);
    EXPECT_FALSE(filter.ShouldImport("/path/to/file1.txt"));
    EXPECT_FALSE(filter.ShouldImport("/path/to/fileA.txt"));
    EXPECT_TRUE(filter.ShouldImport("/path/to/file12.txt"));
}

TEST_F(FileFilterTest, PatternExactMatch) {
    FilterConfig config;
    config.ignore_patterns = {"Thumbs.db"};
    FileFilter filter(config);
    EXPECT_FALSE(filter.ShouldImport("/path/to/Thumbs.db"));
    EXPECT_TRUE(filter.ShouldImport("/path/to/thumbs.db"));  // case-sensitive
}

TEST_F(FileFilterTest, MultipleStarPatterns) {
    FilterConfig config;
    config.ignore_patterns = {"*~", "*.bak", "#*#", "*.swp"};
    FileFilter filter(config);
    EXPECT_FALSE(filter.ShouldImport("/path/to/file~"));
    EXPECT_FALSE(filter.ShouldImport("/path/to/file.bak"));
    EXPECT_FALSE(filter.ShouldImport("/path/to/#autosave#"));
    EXPECT_FALSE(filter.ShouldImport("/path/to/file.swp"));
    EXPECT_TRUE(filter.ShouldImport("/path/to/file.txt"));
}

TEST_F(FileFilterTest, NoIgnorePatterns_AllImported) {
    FilterConfig config;
    config.ignore_patterns = {};
    FileFilter filter(config);
    // Still filters hidden files (dot prefix) and tilde suffix via hard-coded rules
    EXPECT_FALSE(filter.ShouldImport("/path/to/.hidden"));
    EXPECT_FALSE(filter.ShouldImport("/path/to/file~"));
    EXPECT_TRUE(filter.ShouldImport("/path/to/normal.txt"));
}

TEST_F(FileFilterTest, DirectoryTraverse_MatchesPattern) {
    FilterConfig config;
    config.ignore_patterns = {"node_modules", "__pycache__"};
    FileFilter filter(config);
    EXPECT_FALSE(filter.ShouldTraverse("node_modules"));
    EXPECT_FALSE(filter.ShouldTraverse("__pycache__"));
    EXPECT_TRUE(filter.ShouldTraverse("src"));
}

TEST_F(FileFilterTest, FilenameOnlyMatched_NotFullPath) {
    // The pattern should match against filename only, not the full path
    FilterConfig config;
    config.ignore_patterns = {"*.tmp"};
    FileFilter filter(config);
    EXPECT_FALSE(filter.ShouldImport("/very/deep/path/to/data.tmp"));
    EXPECT_TRUE(filter.ShouldImport("/very/deep/path/to/data.txt"));
}

TEST_F(FileFilterTest, AllowedExtensionsMixedCase) {
    FilterConfig config;
    config.allowed_extensions = {".PDF", ".TXT"};
    FileFilter filter(config);
    EXPECT_TRUE(filter.ShouldImport("/path/to/doc.pdf"));
    EXPECT_TRUE(filter.ShouldImport("/path/to/doc.PDF"));
    EXPECT_TRUE(filter.ShouldImport("/path/to/readme.txt"));
    EXPECT_TRUE(filter.ShouldImport("/path/to/readme.TXT"));
    EXPECT_FALSE(filter.ShouldImport("/path/to/image.png"));
}

TEST_F(FileFilterTest, FileWithNoExtension) {
    FilterConfig config;
    config.allowed_extensions = {".pdf"};
    FileFilter filter(config);
    EXPECT_FALSE(filter.ShouldImport("/path/to/Makefile"));
}

TEST_F(FileFilterTest, FileWithMultipleExtensions) {
    FilterConfig config;
    config.allowed_extensions = {".gz"};
    FileFilter filter(config);
    EXPECT_TRUE(filter.ShouldImport("/path/to/archive.tar.gz"));
    EXPECT_FALSE(filter.ShouldImport("/path/to/archive.tar.bz2"));
}

TEST_F(FileFilterTest, RootSlashPath) {
    EXPECT_FALSE(filter_->ShouldImport("/"));
}

TEST_F(FileFilterTest, DoubleExtensionFile) {
    FilterConfig config;
    config.ignore_patterns = {"*.tar.gz"};
    FileFilter filter(config);
    EXPECT_FALSE(filter.ShouldImport("/path/to/archive.tar.gz"));
    EXPECT_TRUE(filter.ShouldImport("/path/to/archive.tar.bz2"));
}

TEST_F(FileFilterTest, PatternWithMultipleWildcards) {
    FilterConfig config;
    config.ignore_patterns = {"*test*.log"};
    FileFilter filter(config);
    EXPECT_FALSE(filter.ShouldImport("/path/to/my_test_run.log"));
    EXPECT_FALSE(filter.ShouldImport("/path/to/test.log"));
    EXPECT_TRUE(filter.ShouldImport("/path/to/my_test_run.txt"));
}

TEST_F(FileFilterTest, ShouldTraverseWithPattern) {
    FilterConfig config;
    config.ignore_patterns = {"build*"};
    FileFilter filter(config);
    EXPECT_FALSE(filter.ShouldTraverse("build"));
    EXPECT_FALSE(filter.ShouldTraverse("build_output"));
    EXPECT_TRUE(filter.ShouldTraverse("src"));
}

TEST_F(FileFilterTest, FileWithSpacesInName) {
    FilterConfig config;
    config.ignore_patterns = {".*"};
    FileFilter filter(config);
    EXPECT_TRUE(filter.ShouldImport("/path/to/my document.pdf"));
    EXPECT_TRUE(filter.ShouldImport("/path/to/report final v2.docx"));
}

TEST_F(FileFilterTest, UnicodeFileName) {
    FilterConfig config;
    FileFilter filter(config);
    EXPECT_TRUE(filter.ShouldImport("/path/to/\xe6\x96\x87\xe6\xa1\xa3.pdf"));
}

}  // namespace
}  // namespace cortrix
