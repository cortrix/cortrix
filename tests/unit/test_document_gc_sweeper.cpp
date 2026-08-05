#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "cortrix/catalog/batch_result.h"
#include "cortrix/catalog/gc/document_gc_sweeper.h"
#include "cortrix/common/data_types.h"
#include "cortrix/config/config.h"
#include "cortrix/resource/namespace_facade.h"
#include "cortrix/store/cortrix_blob_store.h"
#include "cortrix/store/cortrix_store.h"

#include "ns_pool_test_helper.h"

// OPEN-2 document GC (B① per-Unit layer): Stage 2 hard-delete of expired soft-
// deleted docs over a real namespace pool — asserts the real .raw blob is unlinked, the
// block vectors are MarkDelete'd, and the doc/block rows are removed. Plus
// restore + retention-window + dry_run.
namespace cortrix::catalog::gc {
namespace {

namespace fs = std::filesystem;
using ::testing::_;
using ::testing::Invoke;

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// A router whose ListNamespaces returns a fixed set (the sweeper iterates these).
catalog::BatchResult<std::string> NsList(std::vector<std::string> names) {
    catalog::BatchResult<std::string> r;
    r.results = std::move(names);
    return r;
}

class DocumentGcSweeperTest : public ::testing::Test {
protected:
    void SetUp() override {
        harness_ = std::make_unique<cortrix::test::NsPoolHarness>(
            fs::temp_directory_path() /
            ("docgc_" + std::to_string(reinterpret_cast<uintptr_t>(this))));
        ASSERT_TRUE(harness_->Admit(kNs).ok());

        ON_CALL(router_, ListNamespaces(_))
            .WillByDefault(Invoke([](const catalog::ListNamespacesOptions&) {
                return cortrix::Result<catalog::BatchResult<std::string>>(NsList({kNs}));
            }));
    }

    // Seed a doc with a block (carrying a vector block_id) + a real blob file.
    // Returns the on-disk blob path so the test can assert it disappears.
    std::string SeedDoc(const std::string& doc_id, uint64_t block_id) {
        cortrix::resource::NamespaceFacade facade(harness_->ipool(), kNs);
        EXPECT_TRUE(facade.Acquire().ok());

        CortrixDoc doc;
        doc.doc_id = doc_id;
        doc.source_type = "upload";
        doc.source_path = doc_id + ".txt";
        doc.status = DocStatus::kReady;
        EXPECT_EQ(facade.store().doc_create(doc), 0);

        CortrixBlock block;
        block.block_id = block_id;
        block.doc_id = doc_id;
        block.content_text = "hello";
        block.data = {0x01, 0x02, 0x03};  // blocks.data is BLOB NOT NULL
        EXPECT_EQ(facade.store().block_insert(block), 0);

        const std::string payload = "blobdata";
        EXPECT_EQ(facade.blob().store(kNs, doc_id, payload.data(), payload.size()), 0);
        std::vector<uint8_t> out;
        EXPECT_EQ(facade.blob().load(kNs, doc_id, out), 0) << "blob must exist after seed";
        return "";  // path is internal; we re-check via blob().load below
    }

    bool BlobExists(const std::string& doc_id) {
        cortrix::resource::NamespaceFacade facade(harness_->ipool(), kNs);
        EXPECT_TRUE(facade.Acquire().ok());
        std::vector<uint8_t> out;
        return facade.blob().load(kNs, doc_id, out) == 0;
    }

    int DocStatusOf(const std::string& doc_id) {  // -1 = not found
        cortrix::resource::NamespaceFacade facade(harness_->ipool(), kNs);
        EXPECT_TRUE(facade.Acquire().ok());
        CortrixDoc doc;
        if (facade.store().doc_get(doc_id, doc) != 0) return -1;
        return static_cast<int>(doc.status);
    }

    void SoftDelete(const std::string& doc_id, int64_t deleted_at_ms) {
        cortrix::resource::NamespaceFacade facade(harness_->ipool(), kNs);
        EXPECT_TRUE(facade.Acquire().ok());
        EXPECT_EQ(facade.store().doc_soft_delete(doc_id, deleted_at_ms), 0);
    }

    static constexpr const char* kNs = "sales";
    std::unique_ptr<cortrix::test::NsPoolHarness> harness_;
    ::testing::NiceMock<cortrix::test::MockNSRouter> router_;
    GcConfig cfg_;
};

// Stage 2: a doc soft-deleted past the retention window is hard-deleted — the real
// blob file disappears, the block vector is MarkDelete'd, the rows are gone.
TEST_F(DocumentGcSweeperTest, HardDeletesExpiredSoftDeleteAndUnlinksBlob) {
    SeedDoc("docA", /*block_id=*/4242);
    SoftDelete("docA", NowMs() - 40LL * 24 * 3600 * 1000);  // 40 days ago
    ASSERT_TRUE(BlobExists("docA"));

    cfg_.soft_delete_retention_days = 30;
    DocumentGcSweeper sweeper(&router_, &harness_->ipool(), cfg_);
    auto r = sweeper.RunOnce();
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_EQ(r.value().docs_hard_deleted, 1);
    EXPECT_GE(r.value().blocks_deleted, 1);
    EXPECT_EQ(r.value().blobs_deleted, 1);

    EXPECT_FALSE(BlobExists("docA")) << "the real .raw blob must be unlinked";
    EXPECT_EQ(DocStatusOf("docA"), -1) << "doc row hard-deleted";
    // The block's vector was tombstoned via the FakeIndex.
    const auto& deleted = harness_->fake_index()->deleted_ids();
    EXPECT_NE(std::find(deleted.begin(), deleted.end(), 4242u), deleted.end());
}

// Within the retention window the soft-deleted doc is left intact.
TEST_F(DocumentGcSweeperTest, KeepsSoftDeleteWithinWindow) {
    SeedDoc("docB", 7);
    SoftDelete("docB", NowMs() - 5LL * 24 * 3600 * 1000);  // 5 days ago
    cfg_.soft_delete_retention_days = 30;
    DocumentGcSweeper sweeper(&router_, &harness_->ipool(), cfg_);

    auto r = sweeper.RunOnce();
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value().docs_hard_deleted, 0);
    EXPECT_TRUE(BlobExists("docB"));
    EXPECT_EQ(DocStatusOf("docB"), static_cast<int>(DocStatus::kDeleted));
}

// retention_days=0 makes Stage 2 immediate (single-user mode).
TEST_F(DocumentGcSweeperTest, RetentionZeroIsImmediate) {
    SeedDoc("docC", 9);
    SoftDelete("docC", NowMs());  // just now
    cfg_.soft_delete_retention_days = 0;
    DocumentGcSweeper sweeper(&router_, &harness_->ipool(), cfg_);

    auto r = sweeper.RunOnce();
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value().docs_hard_deleted, 1);
    EXPECT_FALSE(BlobExists("docC"));
}

// Purge bypasses the window: a fresh soft-delete is hard-deleted immediately.
TEST_F(DocumentGcSweeperTest, PurgeBypassesWindow) {
    SeedDoc("docD", 11);
    SoftDelete("docD", NowMs());  // fresh
    cfg_.soft_delete_retention_days = 30;
    DocumentGcSweeper sweeper(&router_, &harness_->ipool(), cfg_);

    auto r = sweeper.Purge();
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value().docs_hard_deleted, 1);
    EXPECT_FALSE(BlobExists("docD"));
}

// dry_run reports but does not delete.
TEST_F(DocumentGcSweeperTest, DryRunMutatesNothing) {
    SeedDoc("docE", 13);
    SoftDelete("docE", NowMs() - 40LL * 24 * 3600 * 1000);
    cfg_.soft_delete_retention_days = 30;
    cfg_.dry_run = true;
    DocumentGcSweeper sweeper(&router_, &harness_->ipool(), cfg_);

    auto r = sweeper.RunOnce();
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value().docs_hard_deleted, 1) << "reports what it would delete";
    EXPECT_TRUE(BlobExists("docE")) << "but does not actually delete";
    EXPECT_EQ(DocStatusOf("docE"), static_cast<int>(DocStatus::kDeleted));
}

// Restore flips a soft-deleted doc back to live (found by searching namespaces).
TEST_F(DocumentGcSweeperTest, RestoreReactivates) {
    SeedDoc("docF", 15);
    SoftDelete("docF", NowMs() - 5LL * 24 * 3600 * 1000);
    DocumentGcSweeper sweeper(&router_, &harness_->ipool(), cfg_);

    auto r = sweeper.Restore({"docF", "ghost"});
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value().succeeded.size(), 1u);
    EXPECT_EQ(r.value().succeeded[0], "docF");
    ASSERT_EQ(r.value().failed.size(), 1u);
    EXPECT_EQ(r.value().failed[0].first, "ghost");
    EXPECT_EQ(DocStatusOf("docF"), static_cast<int>(DocStatus::kReady)) << "restored to live";
}

// CountSoftDeleted reports the cross-NS soft-deleted backlog (for GcStatus).
TEST_F(DocumentGcSweeperTest, CountSoftDeleted) {
    SeedDoc("docG", 17);
    SeedDoc("docH", 18);
    SoftDelete("docG", NowMs());
    DocumentGcSweeper sweeper(&router_, &harness_->ipool(), cfg_);

    auto c = sweeper.CountSoftDeleted();
    ASSERT_TRUE(c.ok());
    EXPECT_EQ(c.value(), 1) << "only docG is soft-deleted";
}

}  // namespace
}  // namespace cortrix::catalog::gc
