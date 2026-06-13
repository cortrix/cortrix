/// @file test_sustained_load.cpp
/// @brief Stability tests: sustained load memory growth, rapid create/delete cycles
///
/// Validates that the system handles sustained workloads without memory leaks,
/// resource exhaustion, or performance degradation. Uses short durations
/// for CI compatibility (30 seconds instead of 5 minutes).

#include <gtest/gtest.h>
#include <thread>
#include <chrono>
#include <filesystem>
#include <atomic>
#include <vector>

#include "cortrix/store/cortrix_store_sqlite.h"
#include "cortrix/common/block_header.h"
#include "cortrix/common/block_types.h"

#if defined(__APPLE__)
#include <mach/mach.h>
#elif defined(__linux__)
#include <fstream>
#include <string>
#endif

namespace cortrix {
namespace {

namespace fs = std::filesystem;

/// Get current process RSS (Resident Set Size) in bytes.
/// Returns 0 if unable to determine.
static size_t GetCurrentRSSBytes() {
#if defined(__APPLE__)
    struct mach_task_basic_info info;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  (task_info_t)&info, &count) == KERN_SUCCESS) {
        return info.resident_size;
    }
    return 0;
#elif defined(__linux__)
    std::ifstream statm("/proc/self/statm");
    if (statm.is_open()) {
        size_t pages = 0;
        statm >> pages;  // first field is total program size in pages
        statm >> pages;  // second field is resident set size in pages
        return pages * sysconf(_SC_PAGESIZE);
    }
    return 0;
#else
    return 0;
#endif
}

class SustainedLoadTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_dir_ = fs::temp_directory_path() /
                    ("cortrix_stab_load_" + std::to_string(getpid()));
        fs::create_directories(test_dir_);
        db_path_ = (test_dir_ / "sustained.db").string();
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

// STAB-LOAD-001: Sustained query load — memory growth < 2x over 30 seconds
TEST_F(SustainedLoadTest, SustainedQueryLoadMemoryGrowth) {
    // Seed data for queries; collect the ULIDs for the read loop (D-I6)
    std::vector<std::string> seeded_ids;
    for (int i = 0; i < 100; ++i) {
        CortrixDoc doc;
        doc.source_type = "load_test";
        doc.source_path = "doc_" + std::to_string(i);
        ASSERT_EQ(store_->doc_create(doc), 0);
        seeded_ids.push_back(doc.doc_id);

        auto blob = BlockBuild(kBlockFile, kLevelL2,
            "sustained load test content " + std::to_string(i), "", "", 0, 0);
        CortrixBlock block;
        block.doc_id = doc.doc_id;
        block.chunk_index = 0;
        block.block_type = 1;
        block.processing_level = 2;
        block.data = blob;
        block.content_text = "sustained load test content " + std::to_string(i);
        ASSERT_EQ(store_->block_insert(block), 0);
    }

    // Measure baseline RSS
    size_t rss_before = GetCurrentRSSBytes();
    int total_queries = 0;
    int error_count = 0;

    // Run sustained queries for 30 seconds (CI-friendly; full run would be 5 min)
    auto start = std::chrono::steady_clock::now();
    auto duration = std::chrono::seconds(30);

    while (std::chrono::steady_clock::now() - start < duration) {
        std::vector<SearchResult> results;
        int ret = store_->search_fulltext("sustained load", 10, results);
        if (ret != 0) {
            ++error_count;
        }
        ++total_queries;

        // Also do doc reads to exercise more code paths
        CortrixDoc doc;
        const std::string& doc_id = seeded_ids[total_queries % seeded_ids.size()];
        store_->doc_get(doc_id, doc);
    }

    size_t rss_after = GetCurrentRSSBytes();

    EXPECT_EQ(error_count, 0) << "Query errors during sustained load";
    EXPECT_GT(total_queries, 100) << "Should have run many queries";

    // Memory growth check (only if we could measure RSS)
    if (rss_before > 0 && rss_after > 0) {
        double growth_ratio = static_cast<double>(rss_after) / rss_before;
        EXPECT_LT(growth_ratio, 2.0)
            << "Memory grew from " << (rss_before / 1024 / 1024) << "MB to "
            << (rss_after / 1024 / 1024) << "MB (ratio: " << growth_ratio << ")";
    }
}

// STAB-LOAD-002: Rapid create/delete cycles — no resource leak
TEST_F(SustainedLoadTest, RapidCreateDeleteCycles) {
    const int kCycles = 500;
    int error_count = 0;

    size_t rss_before = GetCurrentRSSBytes();

    for (int cycle = 0; cycle < kCycles; ++cycle) {
        // Create doc + block
        CortrixDoc doc;
        doc.source_type = "cycle_test";
        doc.source_path = "cycle_" + std::to_string(cycle);
        if (store_->doc_create(doc) != 0) {
            ++error_count;
            continue;
        }

        auto blob = BlockBuild(kBlockFile, kLevelL2,
            "cycle content " + std::to_string(cycle), "", "", 0, 0);
        CortrixBlock block;
        block.doc_id = doc.doc_id;
        block.chunk_index = 0;
        block.block_type = 1;
        block.processing_level = 2;
        block.data = blob;
        block.content_text = "cycle content " + std::to_string(cycle);
        store_->block_insert(block);

        // Delete immediately
        if (store_->doc_delete(doc.doc_id) != 0) {
            ++error_count;
        }
    }

    EXPECT_EQ(error_count, 0) << "Errors during create/delete cycles";

    // After all cycles, store should be empty
    int64_t doc_count = 0, block_count = 0;
    store_->doc_count(&doc_count);
    store_->block_count(&block_count);
    EXPECT_EQ(doc_count, 0) << "Leaked docs after create/delete cycles";
    EXPECT_EQ(block_count, 0) << "Leaked blocks after create/delete cycles";

    // Memory growth check
    size_t rss_after = GetCurrentRSSBytes();
    if (rss_before > 0 && rss_after > 0) {
        double growth_ratio = static_cast<double>(rss_after) / rss_before;
        EXPECT_LT(growth_ratio, 2.0)
            << "Memory leak detected: " << (rss_before / 1024 / 1024) << "MB -> "
            << (rss_after / 1024 / 1024) << "MB";
    }
}

}  // namespace
}  // namespace cortrix
