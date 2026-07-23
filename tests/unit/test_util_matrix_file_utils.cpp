// Exhaustive deterministic matrices for connector/file_utils.h pure path helpers
// (GetBasename / GetExtension / DetectMimeType) plus temp-file backed size/hash/
// type probes. Asserts only against the documented behavior of std::filesystem
// based helpers (extension is the last dot segment; dotfiles like ".bashrc" have
// no extension; multi-dot files take the final segment; matching is lowercased).
//
// SUITE PREFIX: FileUtilsUtilMatrix* (globally unique; does not collide with the
// existing FileUtilsTest in test_file_utils.cpp).
#include <gtest/gtest.h>

#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <tuple>
#include <vector>

#include "cortrix/connector/file_utils.h"

namespace cortrix {
namespace {

// ---------------- GetExtension matrix ----------------
// Each row: input filename/path -> expected lowercase extension WITHOUT dot.
struct ExtCase {
    std::string input;
    std::string expected;
};

class FileUtilsUtilMatrixExt : public ::testing::TestWithParam<ExtCase> {};

TEST_P(FileUtilsUtilMatrixExt, Extension) {
    const ExtCase& c = GetParam();
    EXPECT_EQ(GetExtension(c.input), c.expected) << "input=" << c.input;
}

INSTANTIATE_TEST_SUITE_P(
    Cases, FileUtilsUtilMatrixExt,
    ::testing::Values(
        // Simple extensions.
        ExtCase{"file.txt", "txt"},
        ExtCase{"file.pdf", "pdf"},
        ExtCase{"file.md", "md"},
        ExtCase{"file.json", "json"},
        ExtCase{"archive.tar", "tar"},
        // Case normalization.
        ExtCase{"FILE.TXT", "txt"},
        ExtCase{"File.PdF", "pdf"},
        ExtCase{"DATA.JSON", "json"},
        ExtCase{"Photo.JPG", "jpg"},
        ExtCase{"Photo.JPEG", "jpeg"},
        // Multi-dot: only the final segment is the extension.
        ExtCase{"archive.tar.gz", "gz"},
        ExtCase{"my.file.name.txt", "txt"},
        ExtCase{"a.b.c.d", "d"},
        ExtCase{"backup.2024.01.zip", "zip"},
        // No extension at all.
        ExtCase{"README", ""},
        ExtCase{"Makefile", ""},
        ExtCase{"noext", ""},
        ExtCase{"folder/binary", ""},
        // Dotfiles: a leading-dot name has no extension per std::filesystem.
        ExtCase{".bashrc", ""},
        ExtCase{".gitignore", ""},
        ExtCase{".env", ""},
        ExtCase{".vimrc", ""},
        // Dotfile WITH a real extension after the leading dot.
        ExtCase{".config.json", "json"},
        ExtCase{".hidden.txt", "txt"},
        // Trailing dot -> empty extension.
        ExtCase{"file.", ""},
        ExtCase{"name.", ""},
        // Full paths: extension comes from the final component.
        ExtCase{"/home/user/docs/report.pdf", "pdf"},
        ExtCase{"/a/b/c/data.CSV", "csv"},
        ExtCase{"relative/path/to/file.html", "html"},
        ExtCase{"/var/log/app.log", "log"},
        // Path component with no extension.
        ExtCase{"/usr/bin/python", ""},
        ExtCase{"/etc/hosts", ""},
        // Empty string.
        ExtCase{"", ""},
        // Unicode base name with extension (CJK via \x escapes, ASCII source).
        ExtCase{"\xE6\x96\x87\xE6\xA1\xA3.pdf", "pdf"},
        ExtCase{"\xE6\x95\xB0\xE6\x8D\xAE.csv", "csv"},
        ExtCase{"\xE6\x8A\xA5\xE5\x91\x8A.DOCX", "docx"},
        // Mixed digits / hyphen base.
        ExtCase{"v1.2-final.tar", "tar"},
        ExtCase{"2024-report.xlsx", "xlsx"}));

// ---------------- GetBasename matrix ----------------
struct BaseCase {
    std::string input;
    std::string expected;
};

class FileUtilsUtilMatrixBase : public ::testing::TestWithParam<BaseCase> {};

TEST_P(FileUtilsUtilMatrixBase, Basename) {
    const BaseCase& c = GetParam();
    EXPECT_EQ(GetBasename(c.input), c.expected) << "input=" << c.input;
}

INSTANTIATE_TEST_SUITE_P(
    Cases, FileUtilsUtilMatrixBase,
    ::testing::Values(
        BaseCase{"file.txt", "file.txt"},
        BaseCase{"/home/user/file.txt", "file.txt"},
        BaseCase{"/a/b/c/data.json", "data.json"},
        BaseCase{"relative/path/name.md", "name.md"},
        BaseCase{"/usr/bin/python", "python"},
        BaseCase{"just_a_name", "just_a_name"},
        BaseCase{"/etc/hosts", "hosts"},
        BaseCase{".gitignore", ".gitignore"},
        BaseCase{"/home/.bashrc", ".bashrc"},
        BaseCase{"dir/.env", ".env"},
        BaseCase{"archive.tar.gz", "archive.tar.gz"},
        BaseCase{"/x/archive.tar.gz", "archive.tar.gz"},
        BaseCase{"\xE6\x96\x87\xE6\xA1\xA3.pdf", "\xE6\x96\x87\xE6\xA1\xA3.pdf"},
        BaseCase{"/d/\xE6\x95\xB0\xE6\x8D\xAE.csv", "\xE6\x95\xB0\xE6\x8D\xAE.csv"},
        BaseCase{"", ""},
        BaseCase{"a", "a"},
        BaseCase{"./local.txt", "local.txt"},
        BaseCase{"../up/file.bin", "file.bin"},
        BaseCase{"/deeply/nested/dir/structure/leaf.xml", "leaf.xml"},
        BaseCase{"name.", "name."}));

// ---------------- DetectMimeType matrix ----------------
struct MimeCase {
    std::string input;
    std::string expected;
};

class FileUtilsUtilMatrixMime : public ::testing::TestWithParam<MimeCase> {};

TEST_P(FileUtilsUtilMatrixMime, Mime) {
    const MimeCase& c = GetParam();
    EXPECT_EQ(DetectMimeType(c.input), c.expected) << "input=" << c.input;
}

INSTANTIATE_TEST_SUITE_P(
    Cases, FileUtilsUtilMatrixMime,
    ::testing::Values(
        // Known types from the kMimeMap registry.
        MimeCase{"doc.pdf", "application/pdf"},
        MimeCase{"a.doc", "application/msword"},
        MimeCase{"note.txt", "text/plain"},
        MimeCase{"readme.md", "text/markdown"},
        MimeCase{"table.csv", "text/csv"},
        MimeCase{"data.json", "application/json"},
        MimeCase{"feed.xml", "text/xml"},
        MimeCase{"page.html", "text/html"},
        MimeCase{"page.htm", "text/html"},
        MimeCase{"img.png", "image/png"},
        MimeCase{"img.jpg", "image/jpeg"},
        MimeCase{"img.jpeg", "image/jpeg"},
        MimeCase{"anim.gif", "image/gif"},
        MimeCase{"pic.bmp", "image/bmp"},
        MimeCase{"scan.tiff", "image/tiff"},
        MimeCase{"sound.wav", "audio/wav"},
        MimeCase{"song.mp3", "audio/mpeg"},
        MimeCase{"clip.mp4", "video/mp4"},
        MimeCase{"deck.pptx",
                 "application/"
                 "vnd.openxmlformats-officedocument.presentationml.presentation"},
        MimeCase{"sheet.xlsx",
                 "application/"
                 "vnd.openxmlformats-officedocument.spreadsheetml.sheet"},
        MimeCase{"doc.docx",
                 "application/"
                 "vnd.openxmlformats-officedocument.wordprocessingml.document"},
        // Case-insensitive detection.
        MimeCase{"DOC.PDF", "application/pdf"},
        MimeCase{"Note.TXT", "text/plain"},
        MimeCase{"IMG.JPEG", "image/jpeg"},
        MimeCase{"Data.Json", "application/json"},
        // Unknown / no extension -> octet-stream fallback.
        MimeCase{"binary", "application/octet-stream"},
        MimeCase{"file.unknown", "application/octet-stream"},
        MimeCase{"archive.tar.gz", "application/octet-stream"},
        MimeCase{".bashrc", "application/octet-stream"},
        MimeCase{"file.", "application/octet-stream"},
        MimeCase{"", "application/octet-stream"},
        MimeCase{"file.xyz", "application/octet-stream"},
        MimeCase{"data.parquet", "application/octet-stream"},
        // Full paths still resolve via the final extension.
        MimeCase{"/home/user/report.pdf", "application/pdf"},
        MimeCase{"/d/\xE6\x96\x87\xE6\xA1\xA3.txt", "text/plain"}));

// ---------------- Temp-file backed size/type/hash ----------------
class FileUtilsUtilMatrixFs : public ::testing::Test {
protected:
    void SetUp() override {
        namespace fs = std::filesystem;
        // Unique per process + per SetUp: gtest_discover_tests runs each case in its
        // own process, and a fixed dir name + remove_all would let parallel -j2 runs
        // delete each other's files mid-test.
        static std::atomic<unsigned> seq{0};
        dir_ = fs::temp_directory_path() /
               ("cortrix_util_matrix_file_utils_" + std::to_string(getpid()) + "_" +
                std::to_string(seq.fetch_add(1)));
        fs::remove_all(dir_);
        fs::create_directories(dir_);
    }
    void TearDown() override { std::filesystem::remove_all(dir_); }

    std::string Make(const std::string& name, const std::string& content) {
        auto p = dir_ / name;
        std::ofstream out(p, std::ios::binary);
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
        out.close();
        return p.string();
    }
    std::filesystem::path dir_;
};

// GetFileSize over a matrix of byte counts.
TEST_F(FileUtilsUtilMatrixFs, FileSizeMatrix) {
    const std::vector<size_t> sizes = {0, 1, 2, 7, 16, 100, 255, 1024, 4096, 8192,
                                       8193, 10000};
    int i = 0;
    for (size_t n : sizes) {
        std::string content(n, 'x');
        std::string path = Make("size_" + std::to_string(i++) + ".bin", content);
        EXPECT_EQ(GetFileSize(path), static_cast<int64_t>(n)) << "n=" << n;
    }
}

TEST_F(FileUtilsUtilMatrixFs, FileSizeMissingReturnsNegativeOne) {
    EXPECT_EQ(GetFileSize((dir_ / "does_not_exist.bin").string()), -1);
}

// IsRegularFile / IsDirectory over created entries.
TEST_F(FileUtilsUtilMatrixFs, RegularFileVsDirectory) {
    std::string f = Make("regular.txt", "data");
    std::string sub = (dir_ / "subdir").string();
    std::filesystem::create_directories(sub);

    EXPECT_TRUE(IsRegularFile(f));
    EXPECT_FALSE(IsDirectory(f));
    EXPECT_TRUE(IsDirectory(sub));
    EXPECT_FALSE(IsRegularFile(sub));
    // Missing path: both predicates false (error_code swallows).
    std::string missing = (dir_ / "nope").string();
    EXPECT_FALSE(IsRegularFile(missing));
    EXPECT_FALSE(IsDirectory(missing));
}

// ComputeFileHash: structural invariants (64-char lowercase hex, deterministic,
// content-sensitive). We assert structure + relationships, not absolute digests.
TEST_F(FileUtilsUtilMatrixFs, HashStructureAndDeterminism) {
    const std::vector<std::string> contents = {
        "",
        "a",
        "hello world",
        std::string(8192, 'q'),    // exactly one read buffer
        std::string(8193, 'q'),    // buffer + 1 (exercises gcount tail)
        std::string(20000, 'z'),   // multi-buffer
        "\xE6\x96\x87\xE6\xA1\xA3 unicode body"};
    int i = 0;
    for (const auto& c : contents) {
        std::string path = Make("hash_" + std::to_string(i++) + ".bin", c);
        std::string h1;
        ASSERT_EQ(ComputeFileHash(path, h1), 0);
        EXPECT_EQ(h1.size(), 64u) << "content size=" << c.size();
        for (char ch : h1) {
            EXPECT_TRUE((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))
                << "non-hex char in digest";
        }
        // Deterministic: re-hashing the same file yields the same digest.
        std::string h2;
        ASSERT_EQ(ComputeFileHash(path, h2), 0);
        EXPECT_EQ(h1, h2);
    }
}

TEST_F(FileUtilsUtilMatrixFs, HashDistinctContentsDiffer) {
    std::string pa = Make("ha.bin", "content A");
    std::string pb = Make("hb.bin", "content B");
    std::string ha, hb;
    ASSERT_EQ(ComputeFileHash(pa, ha), 0);
    ASSERT_EQ(ComputeFileHash(pb, hb), 0);
    EXPECT_NE(ha, hb);
}

TEST_F(FileUtilsUtilMatrixFs, HashMissingFileReturnsNegativeOne) {
    std::string h;
    EXPECT_EQ(ComputeFileHash((dir_ / "absent.bin").string(), h), -1);
}

}  // namespace
}  // namespace cortrix
