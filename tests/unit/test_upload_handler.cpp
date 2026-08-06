#include <gtest/gtest.h>
#include "cortrix/upload/upload_handler.h"
#include "cortrix/spc/spc_router.h"
#include "cortrix/observability/operation_logger.h"
#include "cortrix/observability/observability_context.h"

#include <memory>

// integration wire⑤c: UploadHandler's methods now take the narrow store/blob windows
// (CortrixStore& / CortrixBlobStore&) instead of a CortrixNamespace&. The fakes
// below are passed DIRECTLY — no namespace/façade needed — which keeps the
// store/blob fault-injection paths (create-fail, blob-fail, find-error,
// doc_get-internal-error) unit-testable. CortrixStore/CortrixBlobStore arrive
// transitively via upload_handler.h.

#include <algorithm>
#include <map>
#include <vector>
#include <cstring>

namespace cortrix {
namespace {

// ============================================================
// Test Fakes
// ============================================================

class FakeStore : public CortrixStore {
public:
    int Open() override { return 0; }
    int Close() override { return 0; }

    int doc_create(CortrixDoc& doc) override {
        if (create_should_fail_) return -1;
        // Mint a ULID-style doc_id (D-I6). Real code mints a real ULID; the fake
        // only needs unique, non-empty, monotonic-looking string ids.
        doc.doc_id = "01JTESTDOC" + std::to_string(next_id_++);
        doc.created_at = "2026-02-15T10:00:00Z";
        doc.updated_at = doc.created_at;
        docs_.push_back(doc);
        return 0;
    }

    int doc_get(const std::string& doc_id, CortrixDoc& doc) override {
        for (const auto& d : docs_) {
            if (d.doc_id == doc_id) { doc = d; return 0; }
        }
        return -2;
    }

    int doc_update_status(const std::string& doc_id, DocStatus status,
                           const std::string& error_msg) override {
        for (auto& d : docs_) {
            if (d.doc_id == doc_id) {
                d.status = status;
                d.error_message = error_msg;
                return 0;
            }
        }
        return -2;
    }

    int doc_delete(const std::string& doc_id) override {
        auto it = std::find_if(docs_.begin(), docs_.end(),
                                [&doc_id](const CortrixDoc& d) { return d.doc_id == doc_id; });
        if (it == docs_.end()) return -2;
        docs_.erase(it);
        delete_called_with_ = doc_id;
        return 0;
    }

    int doc_list_by_status(DocStatus status, std::vector<CortrixDoc>& out) override {
        out.clear();
        for (const auto& d : docs_) {
            if (d.status == status) out.push_back(d);
        }
        return 0;
    }

    int doc_find_by_source(const std::string& source_type,
                            const std::string& source_path,
                            CortrixDoc& doc) override {
        if (find_should_error_) return -1;
        for (const auto& d : docs_) {
            if (d.source_type == source_type && d.source_path == source_path) {
                doc = d;
                return 0;
            }
        }
        return -2;
    }

    int doc_find_by_hash(const std::string&, CortrixDoc&) override { return -2; }

    int doc_count(int64_t* count) override {
        *count = static_cast<int64_t>(docs_.size());
        return 0;
    }

    int block_insert(CortrixBlock& block) override { return 0; }
    int block_get(uint64_t, CortrixBlock&) override { return -2; }
    int block_get_by_doc(const std::string&, std::vector<CortrixBlock>&) override { return 0; }
    int block_delete_by_doc(const std::string&) override { return 0; }
    int block_count(int64_t* count) override { *count = 0; return 0; }
    int search_fulltext(const std::string&, int, std::vector<SearchResult>&) override { return -1; }
    int search_metadata(const std::string&, int, std::vector<SearchResult>&) override { return -1; }

    void set_create_should_fail(bool v) { create_should_fail_ = v; }
    void set_find_should_error(bool v) { find_should_error_ = v; }
    const std::vector<CortrixDoc>& docs() const { return docs_; }
    const std::string& delete_called_with() const { return delete_called_with_; }

private:
    std::vector<CortrixDoc> docs_;
    int64_t next_id_ = 1;
    bool create_should_fail_ = false;
    bool find_should_error_ = false;
    std::string delete_called_with_;
};

class FakeBlobStore : public CortrixBlobStore {
public:
    int store(const std::string& ns, const std::string& doc_id,
              const void* data, size_t len) override {
        if (should_fail_) return -1;
        std::string key = ns + "/" + doc_id;
        blobs_[key] = std::vector<uint8_t>(
            static_cast<const uint8_t*>(data),
            static_cast<const uint8_t*>(data) + len);
        store_count_++;
        return 0;
    }

    int load(const std::string& ns, const std::string& doc_id,
             std::vector<uint8_t>& out) override {
        std::string key = ns + "/" + doc_id;
        auto it = blobs_.find(key);
        if (it == blobs_.end()) return -2;
        out = it->second;
        return 0;
    }

    int del(const std::string& ns, const std::string& doc_id) override {
        std::string key = ns + "/" + doc_id;
        blobs_.erase(key);
        return 0;
    }

    void set_should_fail(bool v) { should_fail_ = v; }
    int store_count() const { return store_count_; }
    size_t blob_count() const { return blobs_.size(); }

private:
    std::map<std::string, std::vector<uint8_t>> blobs_;
    bool should_fail_ = false;
    int store_count_ = 0;
};

class FakeSPCManager : public SPCManager {
public:
    FakeSPCManager() : SPCManager() {}

    Status Submit(std::shared_ptr<SPCTask> task) override {
        if (should_fail_) return Status::Unavailable("queue full");
        submitted_.push_back(task);
        return Status::Ok();
    }

    int CancelBySourcePath(const std::string& path) override {
        cancelled_paths_.push_back(path);
        return 0;
    }

    void Start() override {}
    void Stop() override {}
    size_t QueueSize() const override { return submitted_.size(); }
    SPCStage GetTaskStage(int64_t) const override { return SPCStage::kQueued; }

    void set_should_fail(bool v) { should_fail_ = v; }
    const std::vector<std::shared_ptr<SPCTask>>& submitted() const { return submitted_; }
    const std::vector<std::string>& cancelled_paths() const { return cancelled_paths_; }

private:
    std::vector<std::shared_ptr<SPCTask>> submitted_;
    std::vector<std::string> cancelled_paths_;
    bool should_fail_ = false;
};

// ============================================================
// Test Fixture
// ============================================================

class UploadHandlerTest : public ::testing::Test {
protected:
    void SetUp() override {
        config_.max_file_size = 100 * 1024 * 1024;      // 100MB
        config_.large_file_threshold = 100 * 1024 * 1024; // 100MB

        // Narrow-DI: own the store/blob fakes directly and hand them to the
        // handler methods (no CortrixNamespace wrapper anymore).
        store_owned_ = std::make_unique<FakeStore>();
        blob_owned_ = std::make_unique<FakeBlobStore>();
        store_ = store_owned_.get();
        blob_ = blob_owned_.get();

        spc_ = std::make_unique<FakeSPCManager>();
        handler_ = std::make_unique<UploadHandler>(config_, *spc_);
    }

    UploadRequest MakeRequest(const std::string& filename,
                               const std::string& content,
                               const std::string& mime = "") {
        UploadRequest req;
        req.namespace_name = "test-ns";
        req.filename = filename;
        req.mime_type = mime;
        req.file_data = content.data();
        req.file_size = content.size();
        return req;
    }

    UploadConfig config_;
    std::unique_ptr<FakeStore> store_owned_;
    std::unique_ptr<FakeBlobStore> blob_owned_;
    FakeStore* store_ = nullptr;
    FakeBlobStore* blob_ = nullptr;
    std::unique_ptr<FakeSPCManager> spc_;
    std::unique_ptr<UploadHandler> handler_;
};

// ============================================================
// ComputeContentHash Tests
// ============================================================

// Testing via HandleUpload since ComputeContentHash is private.
// We verify the content_hash in the result matches known SHA-256.

TEST_F(UploadHandlerTest, HandleUpload_NewFile_HashCorrect) {
    std::string content = "hello world";
    auto req = MakeRequest("test.txt", content, "text/plain");
    UploadResult result;

    Status s = handler_->HandleUpload(req, *store_, *blob_, &result);
    ASSERT_TRUE(s.ok()) << s.message();
    // SHA-256 of "hello world"
    EXPECT_EQ(result.content_hash, "b94d27b9934d3e08a52e52d7da7dabfac484efe37a5380ee9088f7ace2efcde9");
}

TEST_F(UploadHandlerTest, HandleUpload_EmptyFile_HashCorrect) {
    std::string content;
    auto req = MakeRequest("empty.txt", content, "text/plain");
    UploadResult result;

    Status s = handler_->HandleUpload(req, *store_, *blob_, &result);
    ASSERT_TRUE(s.ok()) << s.message();
    // SHA-256 of empty string
    EXPECT_EQ(result.content_hash, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

// Additional SHA-256 test vectors (M-TEST-002)
// Reference: NIST FIPS 180-4 and well-known test vectors
TEST_F(UploadHandlerTest, HandleUpload_Hash_SingleChar_a) {
    // SHA-256("a") = ca978112ca1bbdcafac231b39a23dc4da786eff8147c4e72b9807785afee48bb
    std::string content = "a";
    auto req = MakeRequest("a.txt", content, "text/plain");
    UploadResult result;

    Status s = handler_->HandleUpload(req, *store_, *blob_, &result);
    ASSERT_TRUE(s.ok()) << s.message();
    EXPECT_EQ(result.content_hash,
              "ca978112ca1bbdcafac231b39a23dc4da786eff8147c4e72b9807785afee48bb");
}

TEST_F(UploadHandlerTest, HandleUpload_Hash_abc) {
    // SHA-256("abc") = ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad
    std::string content = "abc";
    auto req = MakeRequest("abc.txt", content, "text/plain");
    UploadResult result;

    Status s = handler_->HandleUpload(req, *store_, *blob_, &result);
    ASSERT_TRUE(s.ok()) << s.message();
    EXPECT_EQ(result.content_hash,
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST_F(UploadHandlerTest, HandleUpload_Hash_LongerString) {
    // SHA-256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")
    // = 248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1
    std::string content = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    auto req = MakeRequest("nist.txt", content, "text/plain");
    UploadResult result;

    Status s = handler_->HandleUpload(req, *store_, *blob_, &result);
    ASSERT_TRUE(s.ok()) << s.message();
    EXPECT_EQ(result.content_hash,
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST_F(UploadHandlerTest, HandleUpload_Hash_SingleNullByte) {
    // SHA-256 of a single null byte ("\0")
    // = 6e340b9cffb37a989ca544e6bb780a2c78901d3fb33738768511a30617afa01d
    std::string content(1, '\0');
    auto req = MakeRequest("null.txt", content, "text/plain");
    UploadResult result;

    Status s = handler_->HandleUpload(req, *store_, *blob_, &result);
    ASSERT_TRUE(s.ok()) << s.message();
    EXPECT_EQ(result.content_hash,
              "6e340b9cffb37a989ca544e6bb780a2c78901d3fb33738768511a30617afa01d");
}

TEST_F(UploadHandlerTest, HandleUpload_Hash_DeterministicSameContent) {
    // Same content should always produce the same hash
    std::string content = "deterministic test content 12345";

    auto req1 = MakeRequest("det1.txt", content, "text/plain");
    UploadResult result1;
    Status s1 = handler_->HandleUpload(req1, *store_, *blob_, &result1);
    ASSERT_TRUE(s1.ok());

    // Use a different filename to avoid duplicate detection
    auto req2 = MakeRequest("det2.txt", content, "text/plain");
    UploadResult result2;
    Status s2 = handler_->HandleUpload(req2, *store_, *blob_, &result2);
    ASSERT_TRUE(s2.ok());

    EXPECT_EQ(result1.content_hash, result2.content_hash);
    EXPECT_FALSE(result1.content_hash.empty());
    // SHA-256 hex digest should always be 64 characters
    EXPECT_EQ(result1.content_hash.size(), 64u);
}

TEST_F(UploadHandlerTest, HandleUpload_Hash_DifferentContentDifferentHash) {
    std::string content1 = "content version A";
    std::string content2 = "content version B";

    auto req1 = MakeRequest("diff1.txt", content1, "text/plain");
    UploadResult result1;
    handler_->HandleUpload(req1, *store_, *blob_, &result1);

    auto req2 = MakeRequest("diff2.txt", content2, "text/plain");
    UploadResult result2;
    handler_->HandleUpload(req2, *store_, *blob_, &result2);

    EXPECT_NE(result1.content_hash, result2.content_hash);
}

// ============================================================
// ValidateFileSize Tests (via HandleUpload)
// ============================================================

TEST_F(UploadHandlerTest, HandleUpload_FileSizeUnderLimit) {
    // 99MB worth of data would be impractical, test with exact limit check
    std::string content = "small file";
    auto req = MakeRequest("small.txt", content, "text/plain");
    UploadResult result;

    Status s = handler_->HandleUpload(req, *store_, *blob_, &result);
    EXPECT_TRUE(s.ok());
}

TEST_F(UploadHandlerTest, HandleUpload_FileSizeExactLimit) {
    // Set a small limit for testing
    config_ = {};
    config_.max_file_size = 10;
    config_.large_file_threshold = 100 * 1024 * 1024;
    spc_ = std::make_unique<FakeSPCManager>();
    handler_ = std::make_unique<UploadHandler>(config_, *spc_);

    std::string content = "1234567890"; // exactly 10 bytes
    auto req = MakeRequest("exact.txt", content, "text/plain");
    UploadResult result;

    Status s = handler_->HandleUpload(req, *store_, *blob_, &result);
    EXPECT_TRUE(s.ok()) << s.message();
}

TEST_F(UploadHandlerTest, HandleUpload_FileSizeOverLimit) {
    config_ = {};
    config_.max_file_size = 10;
    config_.large_file_threshold = 100 * 1024 * 1024;
    spc_ = std::make_unique<FakeSPCManager>();
    handler_ = std::make_unique<UploadHandler>(config_, *spc_);

    std::string content = "12345678901"; // 11 bytes, over 10 limit
    auto req = MakeRequest("big.txt", content, "text/plain");
    UploadResult result;

    Status s = handler_->HandleUpload(req, *store_, *blob_, &result);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kInvalidArgument);
    EXPECT_NE(s.message().find("file too large"), std::string::npos);
}

TEST_F(UploadHandlerTest, HandleUpload_ZeroByteFile) {
    std::string content;
    auto req = MakeRequest("zero.txt", content, "text/plain");
    UploadResult result;

    Status s = handler_->HandleUpload(req, *store_, *blob_, &result);
    EXPECT_TRUE(s.ok()) << s.message();
    EXPECT_EQ(result.status, "pending");
}

// ============================================================
// ValidateMimeType Tests (via HandleUpload)
// ============================================================

TEST_F(UploadHandlerTest, HandleUpload_PdfMime) {
    std::string content = "pdf content";
    auto req = MakeRequest("doc.pdf", content, "application/pdf");
    UploadResult result;

    Status s = handler_->HandleUpload(req, *store_, *blob_, &result);
    EXPECT_TRUE(s.ok());
}

TEST_F(UploadHandlerTest, HandleUpload_TxtMime) {
    std::string content = "text content";
    auto req = MakeRequest("doc.txt", content, "text/plain");
    UploadResult result;

    Status s = handler_->HandleUpload(req, *store_, *blob_, &result);
    EXPECT_TRUE(s.ok());
}

TEST_F(UploadHandlerTest, HandleUpload_MarkdownMime) {
    std::string content = "# markdown";
    auto req = MakeRequest("doc.md", content, "text/markdown");
    UploadResult result;

    Status s = handler_->HandleUpload(req, *store_, *blob_, &result);
    EXPECT_TRUE(s.ok());
}

TEST_F(UploadHandlerTest, HandleUpload_DocxMime) {
    std::string content = "docx content";
    auto req = MakeRequest("doc.docx", content,
        "application/vnd.openxmlformats-officedocument.wordprocessingml.document");
    UploadResult result;

    Status s = handler_->HandleUpload(req, *store_, *blob_, &result);
    EXPECT_TRUE(s.ok());
}

TEST_F(UploadHandlerTest, HandleUpload_UnsupportedMime) {
    std::string content = "video data";
    auto req = MakeRequest("video.mp4", content, "video/mp4");
    UploadResult result;

    Status s = handler_->HandleUpload(req, *store_, *blob_, &result);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kInvalidArgument);
    EXPECT_NE(s.message().find("unsupported media type"), std::string::npos);
}

// ============================================================
// ResolveMimeType Tests (via HandleUpload)
// ============================================================

TEST_F(UploadHandlerTest, HandleUpload_MimeInferredFromExtension) {
    // Empty MIME type, should infer from .pdf extension
    std::string content = "pdf data";
    auto req = MakeRequest("report.pdf", content, "");
    UploadResult result;

    Status s = handler_->HandleUpload(req, *store_, *blob_, &result);
    EXPECT_TRUE(s.ok()) << s.message();
}

TEST_F(UploadHandlerTest, HandleUpload_OctetStreamFallbackToExtension) {
    // Generic MIME type, should fallback to extension inference
    std::string content = "md data";
    auto req = MakeRequest("readme.md", content, "application/octet-stream");
    UploadResult result;

    Status s = handler_->HandleUpload(req, *store_, *blob_, &result);
    EXPECT_TRUE(s.ok()) << s.message();
}

// ============================================================
// DeterminePriority Tests (via HandleUpload + SPC task inspection)
// ============================================================

TEST_F(UploadHandlerTest, HandleUpload_SmallFile_P0Priority) {
    std::string content = "small";
    auto req = MakeRequest("small.txt", content, "text/plain");
    UploadResult result;

    handler_->HandleUpload(req, *store_, *blob_, &result);

    ASSERT_EQ(spc_->submitted().size(), 1u);
    EXPECT_EQ(spc_->submitted()[0]->priority, SPCPriority::kP0);
}

TEST_F(UploadHandlerTest, HandleUpload_LargeFile_P2Priority) {
    config_.max_file_size = 200 * 1024 * 1024; // allow up to 200MB
    config_.large_file_threshold = 10;           // threshold at 10 bytes
    spc_ = std::make_unique<FakeSPCManager>();
    handler_ = std::make_unique<UploadHandler>(config_, *spc_);

    std::string content = "large content data"; // > 10 bytes
    auto req = MakeRequest("big.txt", content, "text/plain");
    UploadResult result;

    handler_->HandleUpload(req, *store_, *blob_, &result);

    ASSERT_EQ(spc_->submitted().size(), 1u);
    EXPECT_EQ(spc_->submitted()[0]->priority, SPCPriority::kP2);
}

TEST_F(UploadHandlerTest, HandleUpload_ExactThreshold_P2Priority) {
    config_.max_file_size = 200 * 1024 * 1024;
    config_.large_file_threshold = 5;
    spc_ = std::make_unique<FakeSPCManager>();
    handler_ = std::make_unique<UploadHandler>(config_, *spc_);

    std::string content = "12345"; // exactly 5 bytes = threshold
    auto req = MakeRequest("exact.txt", content, "text/plain");
    UploadResult result;

    handler_->HandleUpload(req, *store_, *blob_, &result);

    ASSERT_EQ(spc_->submitted().size(), 1u);
    EXPECT_EQ(spc_->submitted()[0]->priority, SPCPriority::kP2);
}

// ============================================================
// HandleUpload — New File Flow
// ============================================================

TEST_F(UploadHandlerTest, HandleUpload_NewFile_Success) {
    std::string content = "new file content";
    auto req = MakeRequest("report.txt", content, "text/plain");
    UploadResult result;

    Status s = handler_->HandleUpload(req, *store_, *blob_, &result);
    ASSERT_TRUE(s.ok()) << s.message();

    EXPECT_FALSE(result.doc_id.empty());
    EXPECT_FALSE(result.content_hash.empty());
    EXPECT_EQ(result.status, "pending");
    EXPECT_FALSE(result.is_duplicate);

    // Verify doc was created
    EXPECT_EQ(store_->docs().size(), 1u);
    EXPECT_EQ(store_->docs()[0].source_type, "http_upload");
    EXPECT_EQ(store_->docs()[0].source_path, "report.txt");
    EXPECT_EQ(store_->docs()[0].status, DocStatus::kPending);

    // Verify blob was stored
    EXPECT_EQ(blob_->store_count(), 1);

    // Verify SPC task was submitted
    ASSERT_EQ(spc_->submitted().size(), 1u);
    EXPECT_EQ(spc_->submitted()[0]->doc_id, result.doc_id);
    EXPECT_EQ(spc_->submitted()[0]->source_type, "http_upload");
    EXPECT_EQ(spc_->submitted()[0]->namespace_name, "test-ns");
    EXPECT_FALSE(spc_->submitted()[0]->is_update);
}

TEST_F(UploadHandlerTest, HandleUpload_WithTitle) {
    std::string content = "content";
    auto req = MakeRequest("file.txt", content, "text/plain");
    req.title = "Custom Title";
    UploadResult result;

    handler_->HandleUpload(req, *store_, *blob_, &result);

    EXPECT_EQ(store_->docs()[0].title, "Custom Title");
}

TEST_F(UploadHandlerTest, HandleUpload_DefaultTitle) {
    std::string content = "content";
    auto req = MakeRequest("myfile.txt", content, "text/plain");
    req.title = "";
    UploadResult result;

    handler_->HandleUpload(req, *store_, *blob_, &result);

    EXPECT_EQ(store_->docs()[0].title, "myfile.txt");
}

TEST_F(UploadHandlerTest, HandleUpload_WithMetadata) {
    std::string content = "content";
    auto req = MakeRequest("file.txt", content, "text/plain");
    req.metadata_json = R"({"department":"finance"})";
    UploadResult result;

    handler_->HandleUpload(req, *store_, *blob_, &result);

    EXPECT_EQ(store_->docs()[0].metadata_json, R"({"department":"finance"})");
}

TEST_F(UploadHandlerTest, HandleUpload_MetadataJsonFlowsToSPCTask) {
    std::string content = "content with metadata";
    auto req = MakeRequest("file.txt", content, "text/plain");
    req.metadata_json = R"({"department":"finance","priority":"high"})";
    UploadResult result;

    handler_->HandleUpload(req, *store_, *blob_, &result);

    // Verify SPC task carries the metadata_json
    ASSERT_EQ(spc_->submitted().size(), 1u);
    EXPECT_EQ(spc_->submitted()[0]->metadata_json,
              R"({"department":"finance","priority":"high"})");
}

TEST_F(UploadHandlerTest, HandleUpload_EmptyMetadataJson_EmptyOnSPCTask) {
    std::string content = "content without metadata";
    auto req = MakeRequest("file.txt", content, "text/plain");
    req.metadata_json = "";
    UploadResult result;

    handler_->HandleUpload(req, *store_, *blob_, &result);

    ASSERT_EQ(spc_->submitted().size(), 1u);
    EXPECT_TRUE(spc_->submitted()[0]->metadata_json.empty());
}

// ============================================================
// HandleUpload — Duplicate Detection
// ============================================================

TEST_F(UploadHandlerTest, HandleUpload_DuplicateHash_Skipped) {
    std::string content = "same content";

    // First upload
    auto req1 = MakeRequest("report.txt", content, "text/plain");
    UploadResult result1;
    handler_->HandleUpload(req1, *store_, *blob_, &result1);
    ASSERT_EQ(result1.status, "pending");
    std::string first_doc_id = result1.doc_id;

    // Second upload - same file, same content
    auto req2 = MakeRequest("report.txt", content, "text/plain");
    UploadResult result2;
    Status s = handler_->HandleUpload(req2, *store_, *blob_, &result2);
    ASSERT_TRUE(s.ok());

    EXPECT_EQ(result2.doc_id, first_doc_id);
    EXPECT_EQ(result2.status, "skipped");
    EXPECT_TRUE(result2.is_duplicate);

    // Only one SPC submission (from first upload)
    EXPECT_EQ(spc_->submitted().size(), 1u);
    // Only one doc in store
    EXPECT_EQ(store_->docs().size(), 1u);
}

// ============================================================
// HandleUpload — Incremental Update
// ============================================================

TEST_F(UploadHandlerTest, HandleUpload_UpdatedHash_Updating) {
    // First upload
    std::string content1 = "version 1";
    auto req1 = MakeRequest("report.txt", content1, "text/plain");
    UploadResult result1;
    handler_->HandleUpload(req1, *store_, *blob_, &result1);
    ASSERT_EQ(result1.status, "pending");

    // Second upload - same filename, different content
    std::string content2 = "version 2";
    auto req2 = MakeRequest("report.txt", content2, "text/plain");
    UploadResult result2;
    Status s = handler_->HandleUpload(req2, *store_, *blob_, &result2);
    ASSERT_TRUE(s.ok());

    EXPECT_NE(result2.doc_id, result1.doc_id);
    EXPECT_EQ(result2.status, "updating");
    EXPECT_FALSE(result2.is_duplicate);
    EXPECT_NE(result2.content_hash, result1.content_hash);

    // CancelBySourcePath was called
    ASSERT_EQ(spc_->cancelled_paths().size(), 1u);
    EXPECT_EQ(spc_->cancelled_paths()[0], "report.txt");

    // Two SPC submissions
    EXPECT_EQ(spc_->submitted().size(), 2u);
    EXPECT_TRUE(spc_->submitted()[1]->is_update);
    EXPECT_EQ(spc_->submitted()[1]->old_doc_id, result1.doc_id);
}

// ============================================================
// HandleUpload — Error / Rollback
// ============================================================

TEST_F(UploadHandlerTest, HandleUpload_DocCreateFail) {
    store_->set_create_should_fail(true);

    std::string content = "content";
    auto req = MakeRequest("file.txt", content, "text/plain");
    UploadResult result;

    Status s = handler_->HandleUpload(req, *store_, *blob_, &result);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kInternal);

    // No blob stored, no SPC submitted
    EXPECT_EQ(blob_->store_count(), 0);
    EXPECT_EQ(spc_->submitted().size(), 0u);
}

TEST_F(UploadHandlerTest, HandleUpload_BlobStoreFail_Rollback) {
    blob_->set_should_fail(true);

    std::string content = "content";
    auto req = MakeRequest("file.txt", content, "text/plain");
    UploadResult result;

    Status s = handler_->HandleUpload(req, *store_, *blob_, &result);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kInternal);

    // Doc was rolled back (deleted)
    EXPECT_EQ(store_->docs().size(), 0u);
    EXPECT_FALSE(store_->delete_called_with().empty());

    // No SPC submitted
    EXPECT_EQ(spc_->submitted().size(), 0u);
}

TEST_F(UploadHandlerTest, HandleUpload_SubmitFail_QueueFull) {
    spc_->set_should_fail(true);

    std::string content = "content";
    auto req = MakeRequest("file.txt", content, "text/plain");
    UploadResult result;

    Status s = handler_->HandleUpload(req, *store_, *blob_, &result);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kUnavailable);
    EXPECT_NE(s.message().find("queue full"), std::string::npos);

    // Doc and blob are preserved (crash recovery will handle)
    EXPECT_EQ(store_->docs().size(), 1u);
    EXPECT_EQ(blob_->store_count(), 1);

    // Result still contains valid data
    EXPECT_FALSE(result.doc_id.empty());
    EXPECT_EQ(result.status, "pending");
}

TEST_F(UploadHandlerTest, HandleUpload_FindExistingDocError_TreatsAsNew) {
    store_->set_find_should_error(true);

    std::string content = "content";
    auto req = MakeRequest("file.txt", content, "text/plain");
    UploadResult result;

    Status s = handler_->HandleUpload(req, *store_, *blob_, &result);
    EXPECT_TRUE(s.ok());
    EXPECT_EQ(result.status, "pending");
    EXPECT_FALSE(result.is_duplicate);
}

// ============================================================
// GetDocumentStatus Tests
// ============================================================

TEST_F(UploadHandlerTest, GetDocumentStatus_Found) {
    // Upload a document first
    std::string content = "content";
    auto req = MakeRequest("file.txt", content, "text/plain");
    UploadResult upload_result;
    handler_->HandleUpload(req, *store_, *blob_, &upload_result);

    CortrixDoc doc;
    Status s = handler_->GetDocumentStatus(*store_, upload_result.doc_id, &doc);
    EXPECT_TRUE(s.ok());
    EXPECT_EQ(doc.doc_id, upload_result.doc_id);
    EXPECT_EQ(doc.source_type, "http_upload");
    EXPECT_EQ(doc.source_path, "file.txt");
}

TEST_F(UploadHandlerTest, GetDocumentStatus_NotFound) {
    CortrixDoc doc;
    Status s = handler_->GetDocumentStatus(*store_, "01JTESTDOC0000000000000999", &doc);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kNotFound);
}

// ============================================================
// CreateSpcTask Tests (verified through submitted tasks)
// ============================================================

TEST_F(UploadHandlerTest, CreateSpcTask_NewUpload) {
    std::string content = "content";
    auto req = MakeRequest("file.pdf", content, "application/pdf");
    UploadResult result;

    handler_->HandleUpload(req, *store_, *blob_, &result);

    ASSERT_EQ(spc_->submitted().size(), 1u);
    auto& task = spc_->submitted()[0];
    EXPECT_EQ(task->namespace_name, "test-ns");
    EXPECT_EQ(task->source_path, "file.pdf");
    EXPECT_EQ(task->source_type, "http_upload");
    EXPECT_EQ(task->mime_type, "application/pdf");
    EXPECT_EQ(task->stage, SPCStage::kQueued);
    EXPECT_FALSE(task->is_update);
    EXPECT_TRUE(task->old_doc_id.empty());
}

TEST_F(UploadHandlerTest, CreateSpcTask_UpdateUpload) {
    // First upload
    std::string content1 = "v1";
    auto req1 = MakeRequest("file.txt", content1, "text/plain");
    UploadResult result1;
    handler_->HandleUpload(req1, *store_, *blob_, &result1);

    // Update
    std::string content2 = "v2";
    auto req2 = MakeRequest("file.txt", content2, "text/plain");
    UploadResult result2;
    handler_->HandleUpload(req2, *store_, *blob_, &result2);

    ASSERT_EQ(spc_->submitted().size(), 2u);
    auto& task = spc_->submitted()[1];
    EXPECT_TRUE(task->is_update);
    EXPECT_EQ(task->old_doc_id, result1.doc_id);
}

// ============================================================
// SPCRouter static method tests (used by UploadHandler)
// ============================================================

TEST(SPCRouterTest, InferMimeType_Pdf) {
    EXPECT_EQ(SPCRouter::InferMimeType("report.pdf"), "application/pdf");
}

TEST(SPCRouterTest, InferMimeType_Txt) {
    EXPECT_EQ(SPCRouter::InferMimeType("notes.txt"), "text/plain");
}

TEST(SPCRouterTest, InferMimeType_Markdown) {
    EXPECT_EQ(SPCRouter::InferMimeType("readme.md"), "text/markdown");
}

TEST(SPCRouterTest, InferMimeType_NoExtension) {
    EXPECT_EQ(SPCRouter::InferMimeType("README"), "application/octet-stream");
}

TEST(SPCRouterTest, IsSupported_Pdf) {
    EXPECT_TRUE(SPCRouter::IsSupported("application/pdf"));
}

TEST(SPCRouterTest, IsSupported_TextPlain) {
    EXPECT_TRUE(SPCRouter::IsSupported("text/plain"));
}

TEST(SPCRouterTest, IsSupported_VideoMp4) {
    EXPECT_FALSE(SPCRouter::IsSupported("video/mp4"));
}

TEST(SPCRouterTest, IsSupported_Empty) {
    EXPECT_FALSE(SPCRouter::IsSupported(""));
}

// ============================================================
// SPCRouter::InferProcessingLevel Tests (routing table)
// ============================================================

TEST(SPCRouterTest, InferProcessingLevel_TempFile_L0) {
    EXPECT_EQ(SPCRouter::InferProcessingLevel("application/x-temp"), 0);
}

TEST(SPCRouterTest, InferProcessingLevel_Executable_L0) {
    EXPECT_EQ(SPCRouter::InferProcessingLevel("application/x-executable"), 0);
}

TEST(SPCRouterTest, InferProcessingLevel_Font_L0) {
    EXPECT_EQ(SPCRouter::InferProcessingLevel("font/ttf"), 0);
}

TEST(SPCRouterTest, InferProcessingLevel_Pdf_L3) {
    EXPECT_EQ(SPCRouter::InferProcessingLevel("application/pdf"), 3);
}

TEST(SPCRouterTest, InferProcessingLevel_TextPlain_L3) {
    EXPECT_EQ(SPCRouter::InferProcessingLevel("text/plain"), 3);
}

TEST(SPCRouterTest, InferProcessingLevel_Unknown_L3) {
    EXPECT_EQ(SPCRouter::InferProcessingLevel("application/octet-stream"), 3);
}

// ============================================================
// SPCRouter expanded MIME type tests (routing table)
// ============================================================

TEST(SPCRouterTest, InferMimeType_Csv) {
    EXPECT_EQ(SPCRouter::InferMimeType("data.csv"), "text/csv");
}

TEST(SPCRouterTest, InferMimeType_Html) {
    EXPECT_EQ(SPCRouter::InferMimeType("index.html"), "text/html");
}

TEST(SPCRouterTest, InferMimeType_Htm) {
    EXPECT_EQ(SPCRouter::InferMimeType("page.htm"), "text/html");
}

TEST(SPCRouterTest, InferMimeType_Json) {
    EXPECT_EQ(SPCRouter::InferMimeType("config.json"), "application/json");
}

TEST(SPCRouterTest, InferMimeType_Xml) {
    EXPECT_EQ(SPCRouter::InferMimeType("feed.xml"), "application/xml");
}

TEST(SPCRouterTest, InferMimeType_Xlsx) {
    EXPECT_EQ(SPCRouter::InferMimeType("report.xlsx"),
              "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet");
}

TEST(SPCRouterTest, InferMimeType_Xls) {
    EXPECT_EQ(SPCRouter::InferMimeType("data.xls"), "application/vnd.ms-excel");
}

TEST(SPCRouterTest, InferMimeType_Pptx) {
    EXPECT_EQ(SPCRouter::InferMimeType("slides.pptx"),
              "application/vnd.openxmlformats-officedocument.presentationml.presentation");
}

TEST(SPCRouterTest, InferMimeType_TmpFile_L0) {
    EXPECT_EQ(SPCRouter::InferMimeType("cache.tmp"), "application/x-temp");
}

TEST(SPCRouterTest, InferMimeType_BakFile_L0) {
    EXPECT_EQ(SPCRouter::InferMimeType("backup.bak"), "application/x-temp");
}

TEST(SPCRouterTest, InferMimeType_ExeFile_L0) {
    EXPECT_EQ(SPCRouter::InferMimeType("app.exe"), "application/x-executable");
}

TEST(SPCRouterTest, InferMimeType_DllFile_L0) {
    EXPECT_EQ(SPCRouter::InferMimeType("lib.dll"), "application/x-executable");
}

TEST(SPCRouterTest, InferMimeType_SoFile_L0) {
    EXPECT_EQ(SPCRouter::InferMimeType("lib.so"), "application/x-executable");
}

TEST(SPCRouterTest, InferMimeType_TtfFont_L0) {
    EXPECT_EQ(SPCRouter::InferMimeType("arial.ttf"), "font/ttf");
}

TEST(SPCRouterTest, InferMimeType_OtfFont_L0) {
    EXPECT_EQ(SPCRouter::InferMimeType("font.otf"), "font/ttf");
}

TEST(SPCRouterTest, InferMimeType_WoffFont_L0) {
    EXPECT_EQ(SPCRouter::InferMimeType("font.woff"), "font/ttf");
}

TEST(SPCRouterTest, IsSupported_Csv) {
    EXPECT_TRUE(SPCRouter::IsSupported("text/csv"));
}

TEST(SPCRouterTest, IsSupported_Html) {
    EXPECT_TRUE(SPCRouter::IsSupported("text/html"));
}

TEST(SPCRouterTest, IsSupported_Json) {
    EXPECT_TRUE(SPCRouter::IsSupported("application/json"));
}

TEST(SPCRouterTest, IsSupported_Xml) {
    EXPECT_TRUE(SPCRouter::IsSupported("application/xml"));
}

TEST(SPCRouterTest, IsSupported_TextXml) {
    EXPECT_TRUE(SPCRouter::IsSupported("text/xml"));
}

TEST(SPCRouterTest, IsSupported_Xlsx) {
    EXPECT_TRUE(SPCRouter::IsSupported(
        "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"));
}

TEST(SPCRouterTest, IsSupported_Xls) {
    EXPECT_TRUE(SPCRouter::IsSupported("application/vnd.ms-excel"));
}

TEST(SPCRouterTest, IsSupported_TempNotSupported) {
    EXPECT_FALSE(SPCRouter::IsSupported("application/x-temp"));
}

TEST(SPCRouterTest, IsSupported_ExeNotSupported) {
    EXPECT_FALSE(SPCRouter::IsSupported("application/x-executable"));
}

TEST(SPCRouterTest, IsSupported_FontNotSupported) {
    EXPECT_FALSE(SPCRouter::IsSupported("font/ttf"));
}

// ============================================================
// Upload Handler — processing_level set on SPCTask
// ============================================================

TEST_F(UploadHandlerTest, HandleUpload_SpcTask_ProcessingLevel_TextPlain_L3) {
    std::string content = "content";
    auto req = MakeRequest("file.txt", content, "text/plain");
    UploadResult result;
    handler_->HandleUpload(req, *store_, *blob_, &result);

    ASSERT_EQ(spc_->submitted().size(), 1u);
    // text/plain should get L3
    EXPECT_EQ(spc_->submitted()[0]->processing_level, 3);
}

TEST_F(UploadHandlerTest, HandleUpload_SpcTask_ProcessingLevel_Pdf_L3) {
    std::string content = "pdf data";
    auto req = MakeRequest("doc.pdf", content, "application/pdf");
    UploadResult result;
    handler_->HandleUpload(req, *store_, *blob_, &result);

    ASSERT_EQ(spc_->submitted().size(), 1u);
    EXPECT_EQ(spc_->submitted()[0]->processing_level, 3);
}

// ============================================================
// GetDocumentStatus - internal error path (rc != 0 && rc != -2)
// ============================================================

class FakeStoreWithDocGetError : public CortrixStore {
public:
    int Open() override { return 0; }
    int Close() override { return 0; }
    int doc_create(CortrixDoc& doc) override { return 0; }
    int doc_get(const std::string& doc_id, CortrixDoc& doc) override { return -3; /* internal error */ }
    int doc_update_status(const std::string&, DocStatus, const std::string&) override { return 0; }
    int doc_delete(const std::string&) override { return 0; }
    int doc_list_by_status(DocStatus, std::vector<CortrixDoc>&) override { return 0; }
    int doc_find_by_source(const std::string&, const std::string&, CortrixDoc&) override { return -2; }
    int doc_find_by_hash(const std::string&, CortrixDoc&) override { return -2; }
    int doc_count(int64_t* count) override { *count = 0; return 0; }
    int block_insert(CortrixBlock&) override { return 0; }
    int block_get(uint64_t, CortrixBlock&) override { return -2; }
    int block_get_by_doc(const std::string&, std::vector<CortrixBlock>&) override { return 0; }
    int block_delete_by_doc(const std::string&) override { return 0; }
    int block_count(int64_t* count) override { *count = 0; return 0; }
    int search_fulltext(const std::string&, int, std::vector<SearchResult>&) override { return -1; }
    int search_metadata(const std::string&, int, std::vector<SearchResult>&) override { return -1; }
};

TEST(UploadHandlerExtraTest, GetDocumentStatus_InternalError) {
    UploadConfig config;
    FakeSPCManager spc;
    UploadHandler handler(config, spc);

    // Narrow-DI: pass the failing store window directly (doc_get -> rc -3).
    FakeStoreWithDocGetError store;

    CortrixDoc doc;
    Status s = handler.GetDocumentStatus(store, "01JTESTDOC0000000000000042", &doc);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kInternal);
    EXPECT_NE(s.message().find("failed to query"), std::string::npos);
}

// ============================================================
// UploadHandler - image MIME types supported
// ============================================================

TEST_F(UploadHandlerTest, HandleUpload_ImagePng) {
    std::string content = "png data";
    auto req = MakeRequest("photo.png", content, "image/png");
    UploadResult result;
    Status s = handler_->HandleUpload(req, *store_, *blob_, &result);
    EXPECT_TRUE(s.ok());
}

TEST_F(UploadHandlerTest, HandleUpload_ImageJpeg) {
    std::string content = "jpeg data";
    auto req = MakeRequest("photo.jpg", content, "image/jpeg");
    UploadResult result;
    Status s = handler_->HandleUpload(req, *store_, *blob_, &result);
    EXPECT_TRUE(s.ok());
}

TEST_F(UploadHandlerTest, HandleUpload_ImageTiff) {
    std::string content = "tiff data";
    auto req = MakeRequest("scan.tiff", content, "image/tiff");
    UploadResult result;
    Status s = handler_->HandleUpload(req, *store_, *blob_, &result);
    EXPECT_TRUE(s.ok());
}

TEST_F(UploadHandlerTest, HandleUpload_ImageBmp) {
    std::string content = "bmp data";
    auto req = MakeRequest("image.bmp", content, "image/bmp");
    UploadResult result;
    Status s = handler_->HandleUpload(req, *store_, *blob_, &result);
    EXPECT_TRUE(s.ok());
}

// ============================================================
// Upload - SPC task processing level for L0-skip MIME types
// ============================================================

TEST_F(UploadHandlerTest, HandleUpload_TempFile_ResolvedToUnsupported) {
    // .tmp resolves to application/x-temp which is unsupported by IsSupported
    std::string content = "temp data";
    auto req = MakeRequest("cache.tmp", content, "");
    UploadResult result;
    Status s = handler_->HandleUpload(req, *store_, *blob_, &result);
    // x-temp is not in IsSupported list, so ValidateMimeType should reject it
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kInvalidArgument);
}

// ============================================================
// Upload - submit failure when updating (covers line 184 status="updating")
// ============================================================

TEST_F(UploadHandlerTest, HandleUpload_UpdateSubmitFail_UpdatingStatus) {
    // First upload succeeds
    std::string content1 = "v1";
    auto req1 = MakeRequest("file.txt", content1, "text/plain");
    UploadResult result1;
    handler_->HandleUpload(req1, *store_, *blob_, &result1);

    // Make SPC fail for second upload
    spc_->set_should_fail(true);

    std::string content2 = "v2";
    auto req2 = MakeRequest("file.txt", content2, "text/plain");
    UploadResult result2;
    Status s = handler_->HandleUpload(req2, *store_, *blob_, &result2);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kUnavailable);
    EXPECT_EQ(result2.status, "updating");
}

// ============================================================
// Operation log — SpcPipeline upload site writes operation_log on success
// ============================================================

// Capturing mock operation logger (mirrors test_engine_instrumentation.cpp).
class CapturingOpLogger : public observability::IOperationLogger {
public:
    std::vector<observability::OperationLogEntry> logged;
    void Log(const observability::OperationLogEntry& e,
             const observability::TraceContext*) override { logged.push_back(e); }
    void BatchLog(const std::vector<observability::OperationLogEntry>&,
                  const observability::TraceContext*) override {}
    Result<observability::OperationLogQueryResult> Query(
        const observability::OperationLogFilter&,
        const observability::TraceContext*) override { return Status::Internal("unused"); }
    void Cleanup() override {}
    observability::OperationLogStats GetStats() override { return {}; }
    observability::HealthStatus Health() override { return {}; }
};

class UploadOpLogTest : public UploadHandlerTest {
protected:
    void SetUp() override {
        UploadHandlerTest::SetUp();
        op_logger_ = std::make_shared<CapturingOpLogger>();
        handler_ = std::make_unique<UploadHandler>(config_, *spc_, op_logger_);
        // Seed the thread-local identity the emitter reads (C1).
        auto& octx = observability::ObservabilityContext::ThreadLocal();
        octx.user_id = "alice";
    }
    void TearDown() override {
        observability::ObservabilityContext::ThreadLocal().user_id.reset();
    }
    std::shared_ptr<CapturingOpLogger> op_logger_;
};

TEST_F(UploadOpLogTest, NewUploadEmitsOperationLog) {
    std::string content = "hello world";
    auto req = MakeRequest("notes.txt", content, "text/plain");
    UploadResult result;
    ASSERT_TRUE(handler_->HandleUpload(req, *store_, *blob_, &result).ok());

    ASSERT_EQ(op_logger_->logged.size(), 1u);
    const auto& e = op_logger_->logged[0];
    EXPECT_EQ(e.action, "upload");
    EXPECT_EQ(e.resource_type, "document");          // SpcPipeline (non-db_import)
    EXPECT_EQ(e.namespace_id, "test-ns");
    EXPECT_EQ(e.resource_id, result.doc_id);         // doc_id is the resource id
    EXPECT_EQ(e.user_id, "alice");                   // from the thread-local context
    ASSERT_TRUE(e.summary.has_value());
    EXPECT_NE(e.summary->find("notes.txt"), std::string::npos);
}

TEST_F(UploadOpLogTest, DuplicateSkipEmitsOperationLog) {
    std::string content = "same bytes";
    auto req = MakeRequest("dup.txt", content, "text/plain");
    UploadResult r1;
    ASSERT_TRUE(handler_->HandleUpload(req, *store_, *blob_, &r1).ok());
    ASSERT_EQ(op_logger_->logged.size(), 1u);

    // Re-upload identical content → dedup skip, still a successful operation.
    UploadResult r2;
    ASSERT_TRUE(handler_->HandleUpload(req, *store_, *blob_, &r2).ok());
    EXPECT_TRUE(r2.is_duplicate);
    ASSERT_EQ(op_logger_->logged.size(), 2u);
    EXPECT_EQ(op_logger_->logged[1].action, "upload");
    EXPECT_EQ(op_logger_->logged[1].resource_id, r1.doc_id);
    ASSERT_TRUE(op_logger_->logged[1].summary.has_value());
    EXPECT_NE(op_logger_->logged[1].summary->find("skipped"), std::string::npos);
}

TEST_F(UploadOpLogTest, SpcSubmitFailureDoesNotEmit) {
    // The doc + blob are saved, but Submit fails → non-Ok Status → success-only
    // operation_log must NOT record it.
    spc_->set_should_fail(true);
    std::string content = "will not queue";
    auto req = MakeRequest("fail.txt", content, "text/plain");
    UploadResult result;
    EXPECT_FALSE(handler_->HandleUpload(req, *store_, *blob_, &result).ok());
    EXPECT_TRUE(op_logger_->logged.empty());
}

TEST_F(UploadOpLogTest, ValidationFailureDoesNotEmit) {
    // Unsupported MIME → early reject before any work → no operation_log.
    auto req = MakeRequest("cache.tmp", "temp data", "");
    UploadResult result;
    EXPECT_FALSE(handler_->HandleUpload(req, *store_, *blob_, &result).ok());
    EXPECT_TRUE(op_logger_->logged.empty());
}

TEST_F(UploadHandlerTest, NullOpLoggerUploadStillSucceeds) {
    // The default 2-arg handler_ (op_logger == null) must run the upload unchanged.
    std::string content = "no logger";
    auto req = MakeRequest("plain.txt", content, "text/plain");
    UploadResult result;
    EXPECT_TRUE(handler_->HandleUpload(req, *store_, *blob_, &result).ok());
    EXPECT_EQ(result.status, "pending");
}

}  // namespace
}  // namespace cortrix
