/// @file test_concurrent_stress.cpp
/// @brief Stability tests: concurrent read/write stress, no crash, no deadlock
///
/// Validates that CortrixStore and related components remain stable under
/// heavy concurrent access — multiple threads performing simultaneous
/// reads, writes, searches, and memory operations without crashes,
/// deadlocks, or data corruption.

#include <gtest/gtest.h>
#include <thread>
#include <filesystem>
#include <atomic>
#include <vector>

#include "cortrix/store/cortrix_store_sqlite.h"
#include "cortrix/common/block_header.h"
#include "cortrix/common/block_types.h"

namespace cortrix {
namespace {

namespace fs = std::filesystem;

// ============================================================
// Store-Level Concurrent Stress
// ============================================================

class ConcurrentStressTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_dir_ = fs::temp_directory_path() /
                    ("cortrix_stab_conc_" + std::to_string(getpid()));
        fs::create_directories(test_dir_);
        db_path_ = (test_dir_ / "stress.db").string();
        store_ = std::make_unique<CortrixStoreSqlite>(db_path_);
        ASSERT_EQ(store_->Open(), 0);
    }

    void TearDown() override {
        store_->Close();
        store_.reset();
        std::error_code ec;
        fs::remove_all(test_dir_, ec);
    }

    fs::path test_dir_;
    std::string db_path_;
    std::unique_ptr<CortrixStoreSqlite> store_;
};

// STAB-CONC-001: 10-thread concurrent doc create + read
TEST_F(ConcurrentStressTest, ConcurrentDocCreateAndRead) {
    constexpr int kNumThreads = 10;
    constexpr int kOpsPerThread = 50;
    std::atomic<int> error_count{0};
    std::atomic<int> created_count{0};

    std::vector<std::thread> threads;
    for (int t = 0; t < kNumThreads; ++t) {
        threads.emplace_back([this, t, &error_count, &created_count]() {
            for (int i = 0; i < kOpsPerThread; ++i) {
                CortrixDoc doc;
                doc.source_type = "thread_" + std::to_string(t);
                doc.source_path = "path_" + std::to_string(t) + "_" + std::to_string(i);
                int ret = store_->doc_create(doc);
                if (ret != 0) {
                    error_count.fetch_add(1);
                    continue;
                }
                created_count.fetch_add(1);

                CortrixDoc retrieved;
                ret = store_->doc_get(doc.doc_id, retrieved);
                if (ret != 0) {
                    error_count.fetch_add(1);
                }
            }
        });
    }

    for (auto& th : threads) {
        th.join();
    }

    EXPECT_EQ(error_count.load(), 0) << "Errors during concurrent create/read";

    int64_t total_count = 0;
    store_->doc_count(&total_count);
    EXPECT_EQ(total_count, static_cast<int64_t>(created_count.load()));
}

// STAB-CONC-002: Concurrent doc create + FTS5 search (no crash/deadlock)
TEST_F(ConcurrentStressTest, ConcurrentWriteAndSearch) {
    constexpr int kWriterThreads = 5;
    constexpr int kReaderThreads = 5;
    constexpr int kOpsPerThread = 30;
    std::atomic<int> error_count{0};

    // Pre-seed a doc so there's always something to search
    CortrixDoc seed_doc;
    seed_doc.source_type = "seed";
    seed_doc.source_path = "seed.txt";
    store_->doc_create(seed_doc);

    auto blob = BlockBuild(kBlockFile, kLevelL2, "concurrent test content", "", "", 0, 0);
    CortrixBlock seed_block;
    seed_block.doc_id = seed_doc.doc_id;
    seed_block.chunk_index = 0;
    seed_block.block_type = 1;
    seed_block.processing_level = 2;
    seed_block.data = blob;
    seed_block.content_text = "concurrent test content";
    store_->block_insert(seed_block);

    std::vector<std::thread> threads;

    // Writer threads: create docs + blocks
    for (int t = 0; t < kWriterThreads; ++t) {
        threads.emplace_back([this, t, &error_count]() {
            for (int i = 0; i < kOpsPerThread; ++i) {
                CortrixDoc doc;
                doc.source_type = "writer_" + std::to_string(t);
                doc.source_path = "wp_" + std::to_string(t) + "_" + std::to_string(i);
                if (store_->doc_create(doc) != 0) {
                    error_count.fetch_add(1);
                    continue;
                }

                auto b = BlockBuild(kBlockFile, kLevelL2,
                    "stress content " + std::to_string(i), "", "", 0, 0);
                CortrixBlock block;
                block.doc_id = doc.doc_id;
                block.chunk_index = 0;
                block.block_type = 1;
                block.processing_level = 2;
                block.data = b;
                block.content_text = "stress content " + std::to_string(i);
                if (store_->block_insert(block) != 0) {
                    error_count.fetch_add(1);
                }
            }
        });
    }

    // Reader threads: FTS5 search
    for (int t = 0; t < kReaderThreads; ++t) {
        threads.emplace_back([this, &error_count]() {
            for (int i = 0; i < kOpsPerThread; ++i) {
                std::vector<SearchResult> results;
                int ret = store_->search_fulltext("content", 10, results);
                if (ret != 0) {
                    error_count.fetch_add(1);
                }
            }
        });
    }

    for (auto& th : threads) {
        th.join();
    }

    EXPECT_EQ(error_count.load(), 0) << "Errors during concurrent write+search";
}

// STAB-CONC-003: Concurrent namespace access (multiple namespaces simultaneously)
TEST_F(ConcurrentStressTest, ConcurrentNamespaceAccess) {
    constexpr int kNamespaces = 4;
    constexpr int kOpsPerNs = 30;
    std::atomic<int> error_count{0};

    std::vector<std::unique_ptr<CortrixStoreSqlite>> stores;
    for (int ns = 0; ns < kNamespaces; ++ns) {
        auto path = (test_dir_ / ("ns_" + std::to_string(ns) + ".db")).string();
        auto store = std::make_unique<CortrixStoreSqlite>(path);
        ASSERT_EQ(store->Open(), 0);
        stores.push_back(std::move(store));
    }

    std::vector<std::thread> threads;
    for (int ns = 0; ns < kNamespaces; ++ns) {
        threads.emplace_back([&stores, ns, &error_count]() {
            auto& store = stores[ns];
            for (int i = 0; i < kOpsPerNs; ++i) {
                CortrixDoc doc;
                doc.source_type = "ns_" + std::to_string(ns);
                doc.source_path = "doc_" + std::to_string(i);
                if (store->doc_create(doc) != 0) {
                    error_count.fetch_add(1);
                    continue;
                }

                CortrixDoc retrieved;
                if (store->doc_get(doc.doc_id, retrieved) != 0) {
                    error_count.fetch_add(1);
                }
            }
        });
    }

    for (auto& th : threads) {
        th.join();
    }

    EXPECT_EQ(error_count.load(), 0);

    // Verify isolation: each namespace should have exactly kOpsPerNs docs
    for (int ns = 0; ns < kNamespaces; ++ns) {
        int64_t count = 0;
        stores[ns]->doc_count(&count);
        EXPECT_EQ(count, kOpsPerNs)
            << "Namespace " << ns << " has wrong doc count";
        stores[ns]->Close();
    }
}

}  // namespace
}  // namespace cortrix
