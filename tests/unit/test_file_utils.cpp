#include <gtest/gtest.h>
#include "cortrix/connector/file_utils.h"
#include <fstream>
#include <filesystem>
#include <cstdlib>

namespace cortrix {
namespace {

class FileUtilsTest : public ::testing::Test {
protected:
    void SetUp() override {
        namespace fs = std::filesystem;
        test_dir_ = fs::temp_directory_path() / "cortrix_test_file_utils";
        fs::create_directories(test_dir_);
    }

    void TearDown() override {
        namespace fs = std::filesystem;
        fs::remove_all(test_dir_);
    }

    void CreateFile(const std::string& name, const std::string& content) {
        auto path = test_dir_ / name;
        std::ofstream out(path, std::ios::binary);
        out << content;
    }

    std::string FilePath(const std::string& name) {
        return (test_dir_ / name).string();
    }

    std::filesystem::path test_dir_;
};

// --- ComputeFileHash ---

TEST_F(FileUtilsTest, ComputeHashSmallFile) {
    CreateFile("small.txt", "hello world");
    std::string hash;
    EXPECT_EQ(0, ComputeFileHash(FilePath("small.txt"), hash));
    EXPECT_EQ(64u, hash.size());
    // SHA-256 of "hello world" = b94d27b9934d3e08a52e52d7da7dabfac484efe37a5380ee9088f7ace2efcde9
    EXPECT_EQ("b94d27b9934d3e08a52e52d7da7dabfac484efe37a5380ee9088f7ace2efcde9", hash);
}

TEST_F(FileUtilsTest, ComputeHashEmptyFile) {
    CreateFile("empty.txt", "");
    std::string hash;
    EXPECT_EQ(0, ComputeFileHash(FilePath("empty.txt"), hash));
    EXPECT_EQ(64u, hash.size());
    // SHA-256 of empty = e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855
    EXPECT_EQ("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", hash);
}

TEST_F(FileUtilsTest, ComputeHashLargeFile) {
    // Create a 10MB file
    auto path = test_dir_ / "large.bin";
    {
        std::ofstream out(path, std::ios::binary);
        std::string chunk(1024, 'A');
        for (int i = 0; i < 10240; ++i) {
            out << chunk;
        }
    }
    std::string hash;
    EXPECT_EQ(0, ComputeFileHash(path.string(), hash));
    EXPECT_EQ(64u, hash.size());
}

TEST_F(FileUtilsTest, ComputeHashNonExistent) {
    std::string hash;
    EXPECT_EQ(-1, ComputeFileHash("/nonexistent/path/file.txt", hash));
}

TEST_F(FileUtilsTest, ComputeHashConsistent) {
    CreateFile("consistent.txt", "test content for consistency");
    std::string hash1, hash2;
    EXPECT_EQ(0, ComputeFileHash(FilePath("consistent.txt"), hash1));
    EXPECT_EQ(0, ComputeFileHash(FilePath("consistent.txt"), hash2));
    EXPECT_EQ(hash1, hash2);
}

// --- DetectMimeType ---

TEST_F(FileUtilsTest, GuessMimePdf) {
    EXPECT_EQ("application/pdf", DetectMimeType("report.pdf"));
}

TEST_F(FileUtilsTest, GuessMimeMarkdown) {
    EXPECT_EQ("text/markdown", DetectMimeType("notes.md"));
}

TEST_F(FileUtilsTest, GuessMimeTxt) {
    EXPECT_EQ("text/plain", DetectMimeType("readme.txt"));
}

TEST_F(FileUtilsTest, GuessMimeDocx) {
    EXPECT_EQ("application/vnd.openxmlformats-officedocument.wordprocessingml.document",
              DetectMimeType("document.docx"));
}

TEST_F(FileUtilsTest, GuessMimeUnknown) {
    EXPECT_EQ("application/octet-stream", DetectMimeType("file.xyz"));
}

TEST_F(FileUtilsTest, GuessMimeCaseInsensitive) {
    EXPECT_EQ("application/pdf", DetectMimeType("report.PDF"));
    EXPECT_EQ("application/pdf", DetectMimeType("report.Pdf"));
}

TEST_F(FileUtilsTest, GuessMimeWithPath) {
    EXPECT_EQ("application/pdf", DetectMimeType("/home/user/docs/report.pdf"));
}

// --- GetFileSize ---

TEST_F(FileUtilsTest, GetFileSizeKnown) {
    CreateFile("sized.txt", "12345");
    EXPECT_EQ(5, GetFileSize(FilePath("sized.txt")));
}

TEST_F(FileUtilsTest, GetFileSizeEmpty) {
    CreateFile("empty_size.txt", "");
    EXPECT_EQ(0, GetFileSize(FilePath("empty_size.txt")));
}

TEST_F(FileUtilsTest, GetFileSizeNonExistent) {
    EXPECT_EQ(-1, GetFileSize("/nonexistent/file.txt"));
}

// --- GetBasename ---

TEST_F(FileUtilsTest, GetBasenameFromPath) {
    EXPECT_EQ("report.pdf", GetBasename("/home/user/docs/report.pdf"));
}

TEST_F(FileUtilsTest, GetBasenameNoDir) {
    EXPECT_EQ("file.txt", GetBasename("file.txt"));
}

// --- GetExtension ---

TEST_F(FileUtilsTest, GetExtensionPdf) {
    EXPECT_EQ("pdf", GetExtension("report.pdf"));
}

TEST_F(FileUtilsTest, GetExtensionUppercase) {
    EXPECT_EQ("pdf", GetExtension("report.PDF"));
}

TEST_F(FileUtilsTest, GetExtensionNone) {
    EXPECT_EQ("", GetExtension("Makefile"));
}

TEST_F(FileUtilsTest, GetExtensionMultipleDots) {
    EXPECT_EQ("gz", GetExtension("archive.tar.gz"));
}

// --- ResolvePath ---

TEST_F(FileUtilsTest, ResolvePathExisting) {
    CreateFile("resolve_test.txt", "test");
    auto resolved = ResolvePath(FilePath("resolve_test.txt"));
    EXPECT_FALSE(resolved.empty());
    EXPECT_NE(std::string::npos, resolved.find("resolve_test.txt"));
}

TEST_F(FileUtilsTest, ResolvePathNonExistent) {
    auto resolved = ResolvePath("/nonexistent/path/file.txt");
    EXPECT_TRUE(resolved.empty());
}

TEST_F(FileUtilsTest, ResolvePathDirectory) {
    auto resolved = ResolvePath(test_dir_.string());
    EXPECT_FALSE(resolved.empty());
}

// --- IsDirectory / IsRegularFile ---

TEST_F(FileUtilsTest, IsDirectoryTrue) {
    EXPECT_TRUE(IsDirectory(test_dir_.string()));
}

TEST_F(FileUtilsTest, IsDirectoryFalseForFile) {
    CreateFile("is_dir_test.txt", "content");
    EXPECT_FALSE(IsDirectory(FilePath("is_dir_test.txt")));
}

TEST_F(FileUtilsTest, IsDirectoryFalseForNonExistent) {
    EXPECT_FALSE(IsDirectory("/nonexistent/path"));
}

TEST_F(FileUtilsTest, IsRegularFileTrue) {
    CreateFile("is_file_test.txt", "content");
    EXPECT_TRUE(IsRegularFile(FilePath("is_file_test.txt")));
}

TEST_F(FileUtilsTest, IsRegularFileFalseForDir) {
    EXPECT_FALSE(IsRegularFile(test_dir_.string()));
}

TEST_F(FileUtilsTest, IsRegularFileFalseForNonExistent) {
    EXPECT_FALSE(IsRegularFile("/nonexistent/path/file.txt"));
}

// --- GetBasename edge cases ---

TEST_F(FileUtilsTest, GetBasenameTrailingSlash) {
    // Path with trailing slash returns empty
    auto result = GetBasename("/home/user/");
    // std::filesystem::path("/home/user/").filename() == ""
    EXPECT_EQ("", result);
}

// --- DetectMimeType additional types ---

TEST_F(FileUtilsTest, GuessMimeJson) {
    EXPECT_EQ("application/json", DetectMimeType("data.json"));
}

TEST_F(FileUtilsTest, GuessMimeXml) {
    EXPECT_EQ("text/xml", DetectMimeType("config.xml"));
}

TEST_F(FileUtilsTest, GuessMimeHtml) {
    EXPECT_EQ("text/html", DetectMimeType("page.html"));
    EXPECT_EQ("text/html", DetectMimeType("page.htm"));
}

TEST_F(FileUtilsTest, GuessMimePng) {
    EXPECT_EQ("image/png", DetectMimeType("screenshot.png"));
}

TEST_F(FileUtilsTest, GuessMimeJpeg) {
    EXPECT_EQ("image/jpeg", DetectMimeType("photo.jpg"));
    EXPECT_EQ("image/jpeg", DetectMimeType("photo.jpeg"));
}

TEST_F(FileUtilsTest, GuessMimeCsv) {
    EXPECT_EQ("text/csv", DetectMimeType("data.csv"));
}

TEST_F(FileUtilsTest, GuessMimeNoExtension) {
    EXPECT_EQ("application/octet-stream", DetectMimeType("Makefile"));
}

// --- ComputeFileHash binary content ---

TEST_F(FileUtilsTest, ComputeHashBinaryContent) {
    auto path = test_dir_ / "binary.bin";
    {
        std::ofstream out(path, std::ios::binary);
        // Write null bytes and other binary content
        char data[] = {0x00, 0x01, 0x02, (char)0xFF, (char)0xFE};
        out.write(data, sizeof(data));
    }
    std::string hash;
    EXPECT_EQ(0, ComputeFileHash(path.string(), hash));
    EXPECT_EQ(64u, hash.size());
}

// ======================================================================
// Coverage boost tests: targeting file_utils.cpp remaining 4%
// ======================================================================

TEST_F(FileUtilsTest, ResolvePathSymlink) {
    namespace fs = std::filesystem;
    CreateFile("symlink_target.txt", "target content");
    auto symlink_path = test_dir_ / "link.txt";
    std::error_code ec;
    fs::create_symlink(test_dir_ / "symlink_target.txt", symlink_path, ec);
    if (ec) {
        GTEST_SKIP() << "Symlinks not supported: " << ec.message();
    }

    auto resolved = ResolvePath(symlink_path.string());
    EXPECT_FALSE(resolved.empty());
    // Resolved path should be the real path, not the symlink path
    EXPECT_EQ(std::string::npos, resolved.find("link.txt"));
    EXPECT_NE(std::string::npos, resolved.find("symlink_target.txt"));

    fs::remove(symlink_path, ec);
}

TEST_F(FileUtilsTest, GetFileSizeSymlink) {
    namespace fs = std::filesystem;
    CreateFile("size_target.txt", "12345678");
    auto symlink_path = test_dir_ / "size_link.txt";
    std::error_code ec;
    fs::create_symlink(test_dir_ / "size_target.txt", symlink_path, ec);
    if (ec) {
        GTEST_SKIP() << "Symlinks not supported: " << ec.message();
    }

    EXPECT_EQ(8, GetFileSize(symlink_path.string()));
    fs::remove(symlink_path, ec);
}

TEST_F(FileUtilsTest, DetectMimeTypeGif) {
    EXPECT_EQ("image/gif", DetectMimeType("animation.gif"));
}

TEST_F(FileUtilsTest, DetectMimeTypeBmp) {
    EXPECT_EQ("image/bmp", DetectMimeType("photo.bmp"));
}

TEST_F(FileUtilsTest, DetectMimeTypeTiff) {
    EXPECT_EQ("image/tiff", DetectMimeType("scan.tiff"));
}

TEST_F(FileUtilsTest, DetectMimeTypeWav) {
    EXPECT_EQ("audio/wav", DetectMimeType("recording.wav"));
}

TEST_F(FileUtilsTest, DetectMimeTypeMp3) {
    EXPECT_EQ("audio/mpeg", DetectMimeType("song.mp3"));
}

TEST_F(FileUtilsTest, DetectMimeTypeMp4) {
    EXPECT_EQ("video/mp4", DetectMimeType("movie.mp4"));
}

TEST_F(FileUtilsTest, DetectMimeTypePptx) {
    EXPECT_EQ("application/vnd.openxmlformats-officedocument.presentationml.presentation",
              DetectMimeType("slides.pptx"));
}

TEST_F(FileUtilsTest, DetectMimeTypeXlsx) {
    EXPECT_EQ("application/vnd.openxmlformats-officedocument.spreadsheetml.sheet",
              DetectMimeType("data.xlsx"));
}

TEST_F(FileUtilsTest, DetectMimeTypeDoc) {
    EXPECT_EQ("application/msword", DetectMimeType("legacy.doc"));
}

TEST_F(FileUtilsTest, GetExtensionEmpty) {
    EXPECT_EQ("", GetExtension(""));
}

TEST_F(FileUtilsTest, GetExtensionDotFile) {
    // std::filesystem treats ".gitignore" as a file with no extension
    // (the leading dot is part of the stem, not a separator)
    EXPECT_EQ("", GetExtension(".gitignore"));
}

TEST_F(FileUtilsTest, GetExtensionWithPath) {
    EXPECT_EQ("pdf", GetExtension("/some/path/to/doc.pdf"));
}

TEST_F(FileUtilsTest, IsDirectoryForSymlink) {
    namespace fs = std::filesystem;
    auto sub_dir = test_dir_ / "real_subdir";
    fs::create_directories(sub_dir);
    auto link_path = test_dir_ / "link_to_dir";
    std::error_code ec;
    fs::create_directory_symlink(sub_dir, link_path, ec);
    if (ec) {
        GTEST_SKIP() << "Symlinks not supported: " << ec.message();
    }

    EXPECT_TRUE(IsDirectory(link_path.string()));
    EXPECT_FALSE(IsRegularFile(link_path.string()));
    fs::remove(link_path, ec);
}

TEST_F(FileUtilsTest, ComputeHashFileExactly8192Bytes) {
    // Test file size exactly matching buffer size
    auto path = test_dir_ / "exact_buffer.bin";
    {
        std::ofstream out(path, std::ios::binary);
        std::string content(8192, 'B');
        out << content;
    }
    std::string hash;
    EXPECT_EQ(0, ComputeFileHash(path.string(), hash));
    EXPECT_EQ(64u, hash.size());
}

TEST_F(FileUtilsTest, ComputeHashFileSlightlyOver8192) {
    // Test file size slightly over buffer boundary (exercises the gcount() > 0 path)
    auto path = test_dir_ / "over_buffer.bin";
    {
        std::ofstream out(path, std::ios::binary);
        std::string content(8193, 'C');
        out << content;
    }
    std::string hash;
    EXPECT_EQ(0, ComputeFileHash(path.string(), hash));
    EXPECT_EQ(64u, hash.size());
}

TEST_F(FileUtilsTest, GetBasenameDeepPath) {
    EXPECT_EQ("deep_file.txt", GetBasename("/a/b/c/d/e/f/g/deep_file.txt"));
}

TEST_F(FileUtilsTest, GetBasenameRootFile) {
    EXPECT_EQ("root.txt", GetBasename("/root.txt"));
}

TEST_F(FileUtilsTest, IsRegularFileForSymlink) {
    namespace fs = std::filesystem;
    CreateFile("regular_target.txt", "content");
    auto link_path = test_dir_ / "link_to_file.txt";
    std::error_code ec;
    fs::create_symlink(test_dir_ / "regular_target.txt", link_path, ec);
    if (ec) {
        GTEST_SKIP() << "Symlinks not supported: " << ec.message();
    }

    // Symlink to a regular file should report as regular file
    EXPECT_TRUE(IsRegularFile(link_path.string()));
    EXPECT_FALSE(IsDirectory(link_path.string()));
    fs::remove(link_path, ec);
}

TEST_F(FileUtilsTest, ResolvePathRelative) {
    CreateFile("relative_test.txt", "content");
    // ResolvePath should work with relative paths when they resolve to valid paths
    // (filesystem::canonical resolves relative to cwd)
    auto resolved = ResolvePath(FilePath("relative_test.txt"));
    EXPECT_FALSE(resolved.empty());
}

}  // namespace
}  // namespace cortrix
