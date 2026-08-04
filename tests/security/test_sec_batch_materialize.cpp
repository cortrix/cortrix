// SEC-BATCH — POST /documents/batch inline-content materialization.
//
// The batch surface writes each accepted doc to a server-side file so the F42
// doc-parse worker has something to parse. That write is the only place in the
// ingest path where a request field can become a filesystem path, so it gets the
// same treatment as the upload surface in SEC-INJX-004 (which is safe only
// because the blob store is content-addressed and never uses caller strings as
// paths).
//
// Regression under test: the on-disk basename used to be the caller's doc_id, so
// "../../x" walked out of the materialize dir and an absolute "/tmp/x" made
// std::filesystem::operator/ discard the directory entirely — the content landed
// wherever the caller pointed, overwriting any file the process could write. The
// service now mints the name itself; these tests pin that it stays that way.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "cortrix/server/batch_submit_service.h"
#include "cortrix/server/i_task_submitter.h"

namespace fs = std::filesystem;
using namespace cortrix;

namespace {

class CapturingSubmitter : public server::ITaskSubmitter {
public:
    Result<async::TaskInfo> Submit(const async::SubmitRequest& req) override {
        filepaths.push_back(req.filepath);
        async::TaskInfo info;
        info.task_id = "task-" + std::to_string(filepaths.size());
        return info;
    }
    std::vector<std::string> filepaths;
};

class SecBatchMaterializeTest : public ::testing::Test {
protected:
    void SetUp() override {
        root_ = fs::temp_directory_path() /
                ("cortrix_sec_batch_" + std::to_string(::getpid()));
        fs::remove_all(root_);
        materialize_dir_ = root_ / "data" / "batch_tmp";
        victim_dir_ = root_ / "victim";
        fs::create_directories(materialize_dir_);
        fs::create_directories(victim_dir_);
        // A pre-existing file a traversal payload would try to overwrite.
        std::ofstream(victim_dir_ / "config.yaml") << kVictimOriginal;
    }
    void TearDown() override { fs::remove_all(root_); }

    /// Count of regular files anywhere under `root_` that are NOT inside the
    /// materialize dir and are not the untouched victim file.
    std::vector<fs::path> FilesOutsideMaterializeDir() const {
        std::vector<fs::path> found;
        for (const auto& e : fs::recursive_directory_iterator(root_)) {
            if (!e.is_regular_file()) continue;
            const std::string p = e.path().string();
            if (p.rfind(materialize_dir_.string(), 0) == 0) continue;
            found.push_back(e.path());
        }
        return found;
    }

    static constexpr const char* kVictimOriginal = "original: trusted\n";
    fs::path root_;
    fs::path materialize_dir_;
    fs::path victim_dir_;
};

// SEC-BATCH-001: no doc_id may place the materialized file outside the
// materialize dir, and no pre-existing file may be overwritten.
TEST_F(SecBatchMaterializeTest, DocIdNeverEscapesMaterializeDir) {
    // Mirrors the SEC-INJX-004 payload family (relative, absolute, backslash,
    // nested, NUL truncation) applied to the field that reaches the path here.
    const std::vector<std::string> payloads = {
        "../../victim/ESCAPED",
        "../../../../tmp/cortrix_batch_escape",
        "/tmp/cortrix_batch_abs_escape",
        "..\\..\\victim\\ESCAPED_WIN",
        "nested/../../victim/ESCAPED_NESTED",
        std::string("nul") + '\0' + "../../victim/ESCAPED_NUL",
        // Overwrite attempt against a real existing file.
        "../../victim/config",
    };

    const fs::path abs_escape1 = "/tmp/cortrix_batch_escape.txt";
    const fs::path abs_escape2 = "/tmp/cortrix_batch_abs_escape.txt";
    std::error_code ec;
    fs::remove(abs_escape1, ec);
    fs::remove(abs_escape2, ec);

    CapturingSubmitter submitter;
    server::BatchSubmitService svc(&submitter);
    svc.SetMaterializeDir(materialize_dir_.string());

    for (const std::string& payload : payloads) {
        server::BatchRequest req;
        req.namespace_id = "ns";
        // filename carries an attacker-chosen extension as well.
        req.documents.push_back({payload, "SENTINEL_PAYLOAD", "", "evil.yaml"});
        auto res = svc.Submit(req);
        ASSERT_EQ(res.status, 200) << "payload: " << payload;
    }

    // Nothing was written outside the materialize dir except the victim file,
    // which must still hold its original content.
    for (const fs::path& stray : FilesOutsideMaterializeDir()) {
        EXPECT_EQ(stray, victim_dir_ / "config.yaml")
            << "a batch doc_id materialized a file outside the materialize dir: "
            << stray;
    }
    std::ifstream in(victim_dir_ / "config.yaml");
    const std::string body((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    EXPECT_EQ(body, kVictimOriginal) << "a batch doc_id overwrote an existing file";

    EXPECT_FALSE(fs::exists(abs_escape1));
    EXPECT_FALSE(fs::exists(abs_escape2));

    // Every submitted filepath must resolve inside the materialize dir.
    const fs::path canon_dir = fs::weakly_canonical(materialize_dir_);
    for (const std::string& fp : submitter.filepaths) {
        ASSERT_FALSE(fp.empty());
        const fs::path canon = fs::weakly_canonical(fp);
        EXPECT_EQ(canon.parent_path(), canon_dir) << "escaped filepath: " << fp;
    }
}

// SEC-BATCH-002: the extension is shape-checked, NOT filtered to the parser-backed
// set. Restricting it to known parser types would silently convert every
// unsupported-format submission into a successful plain-text ingest, so an
// unsupported-but-well-formed extension must still reach the parser unchanged and
// fail there exactly as before. Only malformed extensions degrade to ".txt".
TEST_F(SecBatchMaterializeTest, ExtensionIsShapeCheckedButBehaviorPreserving) {
    CapturingSubmitter submitter;
    server::BatchSubmitService svc(&submitter);
    svc.SetMaterializeDir(materialize_dir_.string());

    server::BatchRequest req;
    req.namespace_id = "ns";
    req.documents.push_back({"d1", "x", "", "real.pdf"});      // parser-backed
    req.documents.push_back({"d2", "x", "", "real.MD"});       // lowercased
    req.documents.push_back({"d3", "x", "", "data.json"});     // unsupported, preserved
    req.documents.push_back({"d4", "x", "", "payload.sh"});    // unsupported, preserved
    req.documents.push_back({"d5", "x", "", "payload"});       // no extension
    req.documents.push_back({"d6", "x", "", "weird.a b"});     // malformed shape
    req.documents.push_back(
        {"d7", "x", "", "long." + std::string(40, 'a')});      // over the length cap

    ASSERT_EQ(svc.Submit(req).status, 200);
    ASSERT_EQ(submitter.filepaths.size(), 7u);

    // Preserved verbatim — including the ones the parser will reject.
    EXPECT_EQ(fs::path(submitter.filepaths[0]).extension(), ".pdf");
    EXPECT_EQ(fs::path(submitter.filepaths[1]).extension(), ".md");
    EXPECT_EQ(fs::path(submitter.filepaths[2]).extension(), ".json");
    EXPECT_EQ(fs::path(submitter.filepaths[3]).extension(), ".sh");
    // Absent or malformed — the pre-existing ".txt" fallback.
    EXPECT_EQ(fs::path(submitter.filepaths[4]).extension(), ".txt");
    EXPECT_EQ(fs::path(submitter.filepaths[5]).extension(), ".txt");
    EXPECT_EQ(fs::path(submitter.filepaths[6]).extension(), ".txt");

    // Whatever the extension, the file itself never leaves the materialize dir.
    const fs::path canon_dir = fs::weakly_canonical(materialize_dir_);
    for (const std::string& fp : submitter.filepaths) {
        EXPECT_EQ(fs::weakly_canonical(fp).parent_path(), canon_dir);
    }
}

// SEC-BATCH-003: two namespaces submitting the SAME client doc_id must not share
// an on-disk file. Under the old doc_id-derived naming the second submission
// truncated and rewrote the first one's input, so a queued task could be parsed
// with another tenant's content.
TEST_F(SecBatchMaterializeTest, SameDocIdAcrossNamespacesDoesNotShareAFile) {
    CapturingSubmitter submitter;
    server::BatchSubmitService svc(&submitter);
    svc.SetMaterializeDir(materialize_dir_.string());

    server::BatchRequest a;
    a.namespace_id = "tenant-a";
    a.documents.push_back({"shared-id", "CONTENT_OF_TENANT_A", "", "doc.txt"});
    ASSERT_EQ(svc.Submit(a).status, 200);

    server::BatchRequest b;
    b.namespace_id = "tenant-b";
    b.documents.push_back({"shared-id", "CONTENT_OF_TENANT_B", "", "doc.txt"});
    ASSERT_EQ(svc.Submit(b).status, 200);

    ASSERT_EQ(submitter.filepaths.size(), 2u);
    EXPECT_NE(submitter.filepaths[0], submitter.filepaths[1]);

    // Tenant A's input must still be tenant A's content.
    std::ifstream in(submitter.filepaths[0], std::ios::binary);
    const std::string body((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    EXPECT_EQ(body, "CONTENT_OF_TENANT_A");
}

}  // namespace
