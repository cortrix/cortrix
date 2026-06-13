#include <gtest/gtest.h>
#include "cortrix/store/cortrix_blob_local.h"
#include <filesystem>
#include <fstream>
#include <cstring>
#include <functional>
#include <string>

using namespace cortrix;
namespace fs = std::filesystem;

namespace {
// D-I6: doc_id is now a ULID string. CortrixBlobLocal::MakePath buckets by
// std::hash<std::string>(doc_id) % 256 and uses the doc_id string itself as the
// filename ({doc_id}.raw). These helpers mirror that layout so path-assertion
// tests verify the real bucketing scheme instead of assuming integer rowids.
int BlobBucket(const std::string& doc_id) {
    return static_cast<int>(std::hash<std::string>{}(doc_id) % 256);
}
fs::path BlobPath(const fs::path& base, const std::string& ns,
                  const std::string& doc_id) {
    return base / ("ns_" + ns) / "blobs" / std::to_string(BlobBucket(doc_id)) /
           (doc_id + ".raw");
}
}  // namespace

class BlobLocalTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_dir_ = fs::temp_directory_path() / ("cortrix_blob_test_" + std::to_string(rand()));
        fs::create_directories(test_dir_);
        blob_ = std::make_unique<CortrixBlobLocal>(test_dir_.string());
    }

    void TearDown() override {
        blob_.reset();
        std::error_code ec;
        fs::remove_all(test_dir_, ec);
    }

    fs::path test_dir_;
    std::unique_ptr<CortrixBlobLocal> blob_;
};

// Test 1: Store and Load roundtrip
TEST_F(BlobLocalTest, StoreAndLoadRoundtrip) {
    std::string data = "Hello, blob storage test!";
    int ret = blob_->store("default", "01JTESTDOC0000000000000001", data.data(), data.size());
    ASSERT_EQ(ret, 0);

    std::vector<uint8_t> loaded;
    ret = blob_->load("default", "01JTESTDOC0000000000000001", loaded);
    ASSERT_EQ(ret, 0);
    EXPECT_EQ(loaded.size(), data.size());
    EXPECT_EQ(std::string(loaded.begin(), loaded.end()), data);
}

// Test 2: Path bucket calculation (hash(doc_id) % 256, filename = {doc_id}.raw)
TEST_F(BlobLocalTest, PathBucketCorrect) {
    std::string doc_id = "01JTESTDOC0000000000000257";
    std::string data = "bucket test";
    ASSERT_EQ(blob_->store("ns1", doc_id, data.data(), data.size()), 0);

    // The blob must land in the hash-derived bucket dir with the ULID as filename.
    fs::path expected = BlobPath(test_dir_, "ns1", doc_id);
    EXPECT_TRUE(fs::exists(expected)) << "Expected: " << expected.string();
}

// Test 3: Delete and Load returns not-found
TEST_F(BlobLocalTest, DeleteAndLoadNotFound) {
    std::string data = "delete me";
    ASSERT_EQ(blob_->store("default", "01JTESTDOC0000000000000010", data.data(), data.size()), 0);

    // Delete
    int ret = blob_->del("default", "01JTESTDOC0000000000000010");
    EXPECT_EQ(ret, 0);

    // Load should return -2 (not found)
    std::vector<uint8_t> loaded;
    ret = blob_->load("default", "01JTESTDOC0000000000000010", loaded);
    EXPECT_EQ(ret, -2);
}

// Test 4: Load non-existent blob returns -2
TEST_F(BlobLocalTest, LoadNonExistent) {
    std::vector<uint8_t> loaded;
    int ret = blob_->load("default", "01JTESTDOC0000000000009999", loaded);
    EXPECT_EQ(ret, -2);
}

// Test 5: Delete non-existent returns -2
TEST_F(BlobLocalTest, DeleteNonExistent) {
    int ret = blob_->del("default", "01JTESTDOC0000000000009999");
    EXPECT_EQ(ret, -2);
}

// Test 6: Empty data store and load
TEST_F(BlobLocalTest, EmptyData) {
    ASSERT_EQ(blob_->store("default", "01JTESTDOC0000000000000005", "", 0), 0);

    std::vector<uint8_t> loaded;
    int ret = blob_->load("default", "01JTESTDOC0000000000000005", loaded);
    ASSERT_EQ(ret, 0);
    EXPECT_TRUE(loaded.empty());
}

// Test 7: Binary data roundtrip
TEST_F(BlobLocalTest, BinaryDataRoundtrip) {
    std::vector<uint8_t> binary(1024);
    for (int i = 0; i < 1024; ++i) binary[i] = static_cast<uint8_t>(i & 0xFF);

    ASSERT_EQ(blob_->store("default", "01JTESTDOC0000000000000042", binary.data(), binary.size()), 0);

    std::vector<uint8_t> loaded;
    ASSERT_EQ(blob_->load("default", "01JTESTDOC0000000000000042", loaded), 0);
    EXPECT_EQ(loaded, binary);
}

// Test 8: Namespace isolation
TEST_F(BlobLocalTest, NamespaceIsolation) {
    std::string doc_id = "01JTESTDOC0000000000000001";
    std::string d1 = "data in ns1";
    std::string d2 = "data in ns2";
    ASSERT_EQ(blob_->store("ns1", doc_id, d1.data(), d1.size()), 0);
    ASSERT_EQ(blob_->store("ns2", doc_id, d2.data(), d2.size()), 0);

    std::vector<uint8_t> loaded;
    ASSERT_EQ(blob_->load("ns1", doc_id, loaded), 0);
    EXPECT_EQ(std::string(loaded.begin(), loaded.end()), d1);

    ASSERT_EQ(blob_->load("ns2", doc_id, loaded), 0);
    EXPECT_EQ(std::string(loaded.begin(), loaded.end()), d2);
}

// Test 9: Auto-creates directory structure
TEST_F(BlobLocalTest, AutoCreateDir) {
    std::string doc_id = "01JTESTDOC0000000000000100";
    std::string data = "auto create dir";
    // First store in a fresh namespace
    ASSERT_EQ(blob_->store("newns", doc_id, data.data(), data.size()), 0);

    // Verify the hash-derived bucket directory was created
    fs::path dir = BlobPath(test_dir_, "newns", doc_id).parent_path();
    EXPECT_TRUE(fs::is_directory(dir));
}

// Test 10: Overwrite existing blob
TEST_F(BlobLocalTest, OverwriteExisting) {
    std::string doc_id = "01JTESTDOC0000000000000001";
    std::string v1 = "version 1";
    std::string v2 = "version 2 with more data";
    ASSERT_EQ(blob_->store("default", doc_id, v1.data(), v1.size()), 0);
    ASSERT_EQ(blob_->store("default", doc_id, v2.data(), v2.size()), 0);

    std::vector<uint8_t> loaded;
    ASSERT_EQ(blob_->load("default", doc_id, loaded), 0);
    EXPECT_EQ(std::string(loaded.begin(), loaded.end()), v2);
}

// Test 11: Large blob store and load
TEST_F(BlobLocalTest, LargeBlobRoundtrip) {
    // 1 MB blob
    std::vector<uint8_t> large(1024 * 1024);
    for (size_t i = 0; i < large.size(); ++i) {
        large[i] = static_cast<uint8_t>(i % 251);  // prime pattern
    }

    ASSERT_EQ(blob_->store("default", "01JTESTDOC0000000000000001", large.data(), large.size()), 0);

    std::vector<uint8_t> loaded;
    ASSERT_EQ(blob_->load("default", "01JTESTDOC0000000000000001", loaded), 0);
    EXPECT_EQ(loaded.size(), large.size());
    EXPECT_EQ(loaded, large);
}

// Test 12: Distinct doc_ids in the same namespace do not clobber each other
TEST_F(BlobLocalTest, MultipleSameBucket) {
    // Two distinct ULID doc_ids; whether or not they hash into the same bucket,
    // they must be independently storable and retrievable.
    std::string id1 = "01JTESTDOC0000000000000000";
    std::string id2 = "01JTESTDOC0000000000000256";
    std::string d1 = "data for doc 0";
    std::string d2 = "data for doc 256";
    ASSERT_EQ(blob_->store("default", id1, d1.data(), d1.size()), 0);
    ASSERT_EQ(blob_->store("default", id2, d2.data(), d2.size()), 0);

    std::vector<uint8_t> loaded;
    ASSERT_EQ(blob_->load("default", id1, loaded), 0);
    EXPECT_EQ(std::string(loaded.begin(), loaded.end()), d1);

    ASSERT_EQ(blob_->load("default", id2, loaded), 0);
    EXPECT_EQ(std::string(loaded.begin(), loaded.end()), d2);
}

// Test 13: Namespace with special characters in name
TEST_F(BlobLocalTest, NamespaceSpecialChars) {
    std::string data = "test data";
    // Namespace name with hyphens and underscores (typical real-world usage)
    ASSERT_EQ(blob_->store("my-namespace_v2", "01JTESTDOC0000000000000001", data.data(), data.size()), 0);

    std::vector<uint8_t> loaded;
    ASSERT_EQ(blob_->load("my-namespace_v2", "01JTESTDOC0000000000000001", loaded), 0);
    EXPECT_EQ(std::string(loaded.begin(), loaded.end()), data);
}

// Test 14: Store with nullptr data and zero length
TEST_F(BlobLocalTest, NullDataZeroLen) {
    // Storing nullptr with size 0 - should create an empty file
    ASSERT_EQ(blob_->store("default", "01JTESTDOC0000000000000099", nullptr, 0), 0);

    std::vector<uint8_t> loaded;
    int ret = blob_->load("default", "01JTESTDOC0000000000000099", loaded);
    ASSERT_EQ(ret, 0);
    EXPECT_TRUE(loaded.empty());
}

// ============================================================
// Coverage Boost: Error paths and edge cases
// ============================================================

// Test 15: Store to read-only directory fails
TEST_F(BlobLocalTest, StoreToReadOnlyDir) {
    // Create a blob store with a non-writable base dir
    fs::path readonly_dir = test_dir_ / "readonly";
    fs::create_directories(readonly_dir);
    // Create the doc_id's hash-derived bucket dir first, then make it read-only
    std::string doc_id = "01JTESTDOC0000000000000000";
    fs::path ns_dir = BlobPath(readonly_dir, "ro", doc_id).parent_path();
    fs::create_directories(ns_dir);
    fs::permissions(ns_dir, fs::perms::none);

    CortrixBlobLocal blob(readonly_dir.string());
    std::string data = "test";
    int ret = blob.store("ro", doc_id, data.data(), data.size());
    // Should fail because can't write to the directory
    EXPECT_EQ(ret, -1);

    // Restore permissions for cleanup
    fs::permissions(ns_dir, fs::perms::all);
}

// Test 16: Load from truncated/empty file
TEST_F(BlobLocalTest, LoadEmptyFile) {
    std::string doc_id = "01JTESTDOC0000000000000200";
    std::string data = "some data";
    ASSERT_EQ(blob_->store("default", doc_id, data.data(), data.size()), 0);

    // Truncate the file to 0 bytes
    fs::path path = BlobPath(test_dir_, "default", doc_id);
    ASSERT_TRUE(fs::exists(path));
    // Overwrite with empty file
    std::ofstream(path.string(), std::ios::trunc).close();

    std::vector<uint8_t> loaded;
    int ret = blob_->load("default", doc_id, loaded);
    // Should succeed with empty output (size <= 0 path)
    EXPECT_EQ(ret, 0);
    EXPECT_TRUE(loaded.empty());
}

// Test 17: Delete after delete returns not-found
TEST_F(BlobLocalTest, DeleteTwice) {
    std::string data = "del twice";
    ASSERT_EQ(blob_->store("default", "01JTESTDOC0000000000000300", data.data(), data.size()), 0);
    EXPECT_EQ(blob_->del("default", "01JTESTDOC0000000000000300"), 0);
    EXPECT_EQ(blob_->del("default", "01JTESTDOC0000000000000300"), -2);
}

// Test 18: Store replaces existing blob atomically
TEST_F(BlobLocalTest, StoreAtomicReplace) {
    std::string doc_id = "01JTESTDOC0000000000000400";
    std::string v1 = "version 1 data";
    std::string v2 = "version 2 data which is longer";
    ASSERT_EQ(blob_->store("default", doc_id, v1.data(), v1.size()), 0);
    ASSERT_EQ(blob_->store("default", doc_id, v2.data(), v2.size()), 0);

    std::vector<uint8_t> loaded;
    ASSERT_EQ(blob_->load("default", doc_id, loaded), 0);
    EXPECT_EQ(std::string(loaded.begin(), loaded.end()), v2);

    // Verify no .tmp file left behind
    fs::path blob_dir = BlobPath(test_dir_, "default", doc_id).parent_path();
    for (auto& entry : fs::directory_iterator(blob_dir)) {
        EXPECT_EQ(entry.path().extension(), ".raw")
            << "Found non-.raw file: " << entry.path();
    }
}

// Test 19: MakePath places each doc_id at its hash-derived bucket path,
// using the ULID string itself as the filename ({doc_id}.raw).
TEST_F(BlobLocalTest, BucketCalculation) {
    std::string data = "x";
    for (const std::string& doc_id : {
             std::string("01JTESTDOC0000000000000000"),
             std::string("01JTESTDOC0000000000000255"),
             std::string("01JTESTDOC0000000000000256"),
             std::string("01JTESTDOC0000000000000512")}) {
        ASSERT_EQ(blob_->store("bkt", doc_id, data.data(), data.size()), 0);
        EXPECT_TRUE(fs::exists(BlobPath(test_dir_, "bkt", doc_id)))
            << "Missing blob for doc_id=" << doc_id;
    }
}

// Test 20.5: Store to path where parent dir is read-only (after mkdir success)
// Tests the write failure branch at lines 39-42, 44-47
TEST_F(BlobLocalTest, StoreToPermissionDeniedDir) {
    // Create the target directory structure (doc_id's hash-derived bucket dir)
    std::string doc_id = "01JTESTDOC0000000000000000";
    fs::path ns_dir = BlobPath(test_dir_, "perm", doc_id).parent_path();
    fs::create_directories(ns_dir);

    // Write a first blob successfully
    std::string data1 = "first";
    ASSERT_EQ(blob_->store("perm", doc_id, data1.data(), data1.size()), 0);

    // Now make the directory read-only so temp file creation fails
    fs::permissions(ns_dir, fs::perms::owner_read | fs::perms::owner_exec);

    std::string data2 = "second";
    int ret = blob_->store("perm", doc_id, data2.data(), data2.size());
    EXPECT_EQ(ret, -1);  // Should fail (can't create temp file)

    // Restore permissions for cleanup
    fs::permissions(ns_dir, fs::perms::all);
}

// Test 20: Load after store preserves exact binary content
TEST_F(BlobLocalTest, ExactBinaryPreservation) {
    // Create a buffer with all 256 byte values
    std::vector<uint8_t> data(256);
    for (int i = 0; i < 256; ++i) data[i] = static_cast<uint8_t>(i);

    ASSERT_EQ(blob_->store("default", "01JTESTDOC0000000000000500", data.data(), data.size()), 0);

    std::vector<uint8_t> loaded;
    ASSERT_EQ(blob_->load("default", "01JTESTDOC0000000000000500", loaded), 0);
    ASSERT_EQ(loaded.size(), 256u);
    for (int i = 0; i < 256; ++i) {
        EXPECT_EQ(loaded[i], static_cast<uint8_t>(i)) << "Mismatch at byte " << i;
    }
}
