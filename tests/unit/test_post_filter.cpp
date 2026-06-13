#include <algorithm>
#include <gtest/gtest.h>
#include "cortrix/query/post_filter.h"
#include "cortrix/query/scored_block.h"
#include "cortrix/query/query_request.h"
#include "cortrix/common/data_types.h"

namespace cortrix {
namespace {

// Simple fake CortrixStore for testing PostFilter
class FakeCortrixStore : public CortrixStore {
public:
    // Store test data
    // blocks keyed by block_id (uint64); docs keyed by doc_id (ULID string)
    std::unordered_map<int64_t, CortrixBlock> blocks;
    std::unordered_map<std::string, CortrixDoc> docs;

    int Open() override { return 0; }
    int Close() override { return 0; }

    int doc_create(CortrixDoc&) override { return 0; }
    int doc_get(const std::string& doc_id, CortrixDoc& doc) override {
        auto it = docs.find(doc_id);
        if (it == docs.end()) return -2;
        doc = it->second;
        return 0;
    }
    int doc_update_status(const std::string&, DocStatus, const std::string&) override { return 0; }
    int doc_delete(const std::string&) override { return 0; }
    int doc_list_by_status(DocStatus, std::vector<CortrixDoc>&) override { return 0; }
    int doc_find_by_source(const std::string&, const std::string&, CortrixDoc&) override { return -2; }
    int doc_find_by_hash(const std::string&, CortrixDoc&) override { return -2; }
    int doc_count(int64_t*) override { return 0; }

    int block_insert(CortrixBlock&) override { return 0; }
    int block_get(uint64_t block_id, CortrixBlock& block) override {
        auto it = blocks.find(block_id);
        if (it == blocks.end()) return -2;
        block = it->second;
        return 0;
    }
    int block_get_by_doc(const std::string& doc_id, std::vector<CortrixBlock>& out) override {
        for (const auto& [bid, blk] : blocks) {
            if (blk.doc_id == doc_id) out.push_back(blk);
        }
        // Sort by chunk_index ascending (mimic real SQLite ORDER BY)
        std::sort(out.begin(), out.end(),
                  [](const CortrixBlock& a, const CortrixBlock& b) {
                      return a.chunk_index < b.chunk_index;
                  });
        return 0;
    }
    int block_delete_by_doc(const std::string&) override { return 0; }
    int block_count(int64_t*) override { return 0; }

    int search_fulltext(const std::string&, int, std::vector<SearchResult>&) override { return 0; }
    int search_metadata(const std::string&, int, std::vector<SearchResult>&) override { return -1; }
};

class PostFilterTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Setup test data
        // Doc 1: /contracts/2024/risk.pdf, created 2024-06-01
        CortrixDoc doc1;
        doc1.doc_id = "test-doc-1";
        doc1.source_path = "/contracts/2024/risk.pdf";
        doc1.created_at = "2024-06-01T00:00:00Z";
        store_.docs["test-doc-1"] = doc1;

        // Doc 2: /reports/2025/annual.pdf, created 2025-01-15
        CortrixDoc doc2;
        doc2.doc_id = "test-doc-2";
        doc2.source_path = "/reports/2025/annual.pdf";
        doc2.created_at = "2025-01-15T00:00:00Z";
        store_.docs["test-doc-2"] = doc2;

        // Block 101: doc_id="test-doc-1", type=FILE(1)
        CortrixBlock blk1;
        blk1.block_id = 101;
        blk1.doc_id = "test-doc-1";
        blk1.block_type = 1;  // FILE
        blk1.content_text = "Contract risk assessment clause";
        store_.blocks[101] = blk1;

        // Block 102: doc_id="test-doc-1", type=FILE(1)
        CortrixBlock blk2;
        blk2.block_id = 102;
        blk2.doc_id = "test-doc-1";
        blk2.block_type = 1;  // FILE
        blk2.content_text = "Contract termination clause";
        store_.blocks[102] = blk2;

        // Block 201: doc_id="test-doc-2", type=DATABASE(3)
        CortrixBlock blk3;
        blk3.block_id = 201;
        blk3.doc_id = "test-doc-2";
        blk3.block_type = 3;  // DATABASE
        blk3.content_text = "Annual report summary data";
        store_.blocks[201] = blk3;

        filter_ = std::make_unique<PostFilter>(store_);
    }

    FakeCortrixStore store_;
    std::unique_ptr<PostFilter> filter_;

    std::vector<ScoredBlock> MakeBlocks(std::vector<int64_t> ids) {
        std::vector<ScoredBlock> blocks;
        for (size_t i = 0; i < ids.size(); ++i) {
            ScoredBlock sb;
            sb.block_id = ids[i];
            sb.rrf_score = 1.0f / (1 + static_cast<float>(i));
            sb.hit_routes = kRouteVector;
            blocks.push_back(sb);
        }
        return blocks;
    }
};

// --- No filter ---

TEST_F(PostFilterTest, NoFilter_ReturnsAll) {
    auto blocks = MakeBlocks({101, 102, 201});
    QueryFilter filter;  // empty

    auto results = filter_->Apply(blocks, filter, 10);
    ASSERT_EQ(results.size(), 3u);
    EXPECT_EQ(results[0].block_id, 101);
    EXPECT_EQ(results[1].block_id, 102);
    EXPECT_EQ(results[2].block_id, 201);
}

TEST_F(PostFilterTest, NoFilter_TruncatesToTopK) {
    auto blocks = MakeBlocks({101, 102, 201});
    QueryFilter filter;

    auto results = filter_->Apply(blocks, filter, 2);
    ASSERT_EQ(results.size(), 2u);
    EXPECT_EQ(results[0].block_id, 101);
    EXPECT_EQ(results[1].block_id, 102);
}

// --- Block type filter ---

TEST_F(PostFilterTest, FilterByBlockType_FILE) {
    auto blocks = MakeBlocks({101, 201, 102});
    QueryFilter filter;
    filter.block_types = {"FILE"};

    auto results = filter_->Apply(blocks, filter, 10);
    ASSERT_EQ(results.size(), 2u);
    EXPECT_EQ(results[0].block_id, 101);
    EXPECT_EQ(results[1].block_id, 102);
}

TEST_F(PostFilterTest, FilterByBlockType_DATABASE) {
    auto blocks = MakeBlocks({101, 201, 102});
    QueryFilter filter;
    filter.block_types = {"DATABASE"};

    auto results = filter_->Apply(blocks, filter, 10);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].block_id, 201);
}

TEST_F(PostFilterTest, FilterByBlockType_Multiple) {
    auto blocks = MakeBlocks({101, 201, 102});
    QueryFilter filter;
    filter.block_types = {"FILE", "DATABASE"};

    auto results = filter_->Apply(blocks, filter, 10);
    ASSERT_EQ(results.size(), 3u);
}

// --- Source path filter ---

TEST_F(PostFilterTest, FilterBySourcePath_Glob) {
    auto blocks = MakeBlocks({101, 201, 102});
    QueryFilter filter;
    filter.source_path = "/contracts/*";

    auto results = filter_->Apply(blocks, filter, 10);
    ASSERT_EQ(results.size(), 2u);
    EXPECT_EQ(results[0].block_id, 101);
    EXPECT_EQ(results[1].block_id, 102);
}

TEST_F(PostFilterTest, FilterBySourcePath_Exact) {
    auto blocks = MakeBlocks({101, 201});
    QueryFilter filter;
    filter.source_path = "/reports/2025/annual.pdf";

    auto results = filter_->Apply(blocks, filter, 10);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].block_id, 201);
}

TEST_F(PostFilterTest, FilterBySourcePath_NoMatch) {
    auto blocks = MakeBlocks({101, 201, 102});
    QueryFilter filter;
    filter.source_path = "/nonexistent/*";

    auto results = filter_->Apply(blocks, filter, 10);
    EXPECT_TRUE(results.empty());
}

// --- Created after filter ---

TEST_F(PostFilterTest, FilterByCreatedAfter) {
    auto blocks = MakeBlocks({101, 201, 102});
    QueryFilter filter;
    filter.created_after = "2025-01-01";

    auto results = filter_->Apply(blocks, filter, 10);
    // Only doc2 (created 2025-01-15) passes
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].block_id, 201);
}

// --- Combined filters ---

TEST_F(PostFilterTest, CombinedFilter_TypeAndPath) {
    auto blocks = MakeBlocks({101, 201, 102});
    QueryFilter filter;
    filter.block_types = {"FILE"};
    filter.source_path = "/contracts/*";

    auto results = filter_->Apply(blocks, filter, 10);
    ASSERT_EQ(results.size(), 2u);
}

// --- Missing block handling ---

TEST_F(PostFilterTest, MissingBlock_Skipped) {
    auto blocks = MakeBlocks({101, 999, 201});  // 999 doesn't exist
    QueryFilter filter;

    auto results = filter_->Apply(blocks, filter, 10);
    ASSERT_EQ(results.size(), 2u);
    EXPECT_EQ(results[0].block_id, 101);
    EXPECT_EQ(results[1].block_id, 201);
}

// --- ResultItem fields ---

TEST_F(PostFilterTest, ResultItemFields) {
    auto blocks = MakeBlocks({101});
    blocks[0].hit_routes = kRouteVector | kRouteBM25;
    QueryFilter filter;

    auto results = filter_->Apply(blocks, filter, 10);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].block_id, 101);
    EXPECT_EQ(results[0].doc_id, "test-doc-1");
    EXPECT_EQ(results[0].source_path, "/contracts/2024/risk.pdf");
    EXPECT_EQ(results[0].block_type, "FILE");
    EXPECT_EQ(results[0].chunk_text, "Contract risk assessment clause");
    EXPECT_EQ(results[0].related_blocks_count, 2);  // vector + bm25
    EXPECT_FLOAT_EQ(results[0].score, 1.0f);
}

// --- Empty input ---

TEST_F(PostFilterTest, EmptyBlocks) {
    std::vector<ScoredBlock> blocks;
    QueryFilter filter;
    auto results = filter_->Apply(blocks, filter, 10);
    EXPECT_TRUE(results.empty());
}

// --- Filter removes all ---

TEST_F(PostFilterTest, FilterRemovesAll) {
    auto blocks = MakeBlocks({101, 102});
    QueryFilter filter;
    filter.block_types = {"DATABASE"};  // None of these blocks are DATABASE

    auto results = filter_->Apply(blocks, filter, 10);
    EXPECT_TRUE(results.empty());
}

// --- Comprehensive GlobMatch tests ---
// We test GlobMatch indirectly via PostFilter::Apply with source_path filters

TEST_F(PostFilterTest, GlobMatch_LeadingWildcard_PdfExtension) {
    auto blocks = MakeBlocks({101, 201});
    QueryFilter filter;
    filter.source_path = "*.pdf";

    auto results = filter_->Apply(blocks, filter, 10);
    // Both docs have .pdf extension
    ASSERT_EQ(results.size(), 2u);
}

TEST_F(PostFilterTest, GlobMatch_LeadingWildcard_NoMatch) {
    auto blocks = MakeBlocks({101, 201});
    QueryFilter filter;
    filter.source_path = "*.docx";

    auto results = filter_->Apply(blocks, filter, 10);
    EXPECT_TRUE(results.empty());
}

TEST_F(PostFilterTest, GlobMatch_ContainsPattern) {
    auto blocks = MakeBlocks({101, 201});
    QueryFilter filter;
    filter.source_path = "*2024*";  // Only doc1 has "2024" in path

    auto results = filter_->Apply(blocks, filter, 10);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].source_path, "/contracts/2024/risk.pdf");
}

TEST_F(PostFilterTest, GlobMatch_InteriorWildcard) {
    auto blocks = MakeBlocks({101, 201});
    QueryFilter filter;
    filter.source_path = "/contracts/*.pdf";  // prefix + suffix

    auto results = filter_->Apply(blocks, filter, 10);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].block_id, 101);
}

// Interior wildcard where the prefix matches but the suffix does NOT (the
// "prefix ok && suffix mismatch" false sub-branch of the interior-glob path).
TEST_F(PostFilterTest, GlobMatch_InteriorWildcard_SuffixMismatch) {
    auto blocks = MakeBlocks({101, 201});
    QueryFilter filter;
    filter.source_path = "/contracts/*.docx";  // prefix matches risk.pdf, suffix doesn't

    auto results = filter_->Apply(blocks, filter, 10);
    EXPECT_TRUE(results.empty());
}

// Interior wildcard where prefix+suffix together are longer than the path (the
// size-overflow guard true arm -> false).
TEST_F(PostFilterTest, GlobMatch_InteriorWildcard_PatternLongerThanPath) {
    auto blocks = MakeBlocks({101});
    QueryFilter filter;
    // prefix + suffix length far exceeds "/contracts/2024/risk.pdf".
    filter.source_path = "/this/is/a/very/long/prefix/*/and/an/equally/long/suffix.pdf";

    auto results = filter_->Apply(blocks, filter, 10);
    EXPECT_TRUE(results.empty());
}

// Leading-wildcard suffix longer than the path (the suffix.size() > path.size()
// guard true arm in the "*suffix" branch).
TEST_F(PostFilterTest, GlobMatch_LeadingWildcard_SuffixLongerThanPath) {
    auto blocks = MakeBlocks({101});
    QueryFilter filter;
    filter.source_path = "*aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.pdf";

    auto results = filter_->Apply(blocks, filter, 10);
    EXPECT_TRUE(results.empty());
}

TEST_F(PostFilterTest, GlobMatch_ExactMatch_FullPath) {
    auto blocks = MakeBlocks({101, 201});
    QueryFilter filter;
    filter.source_path = "/contracts/2024/risk.pdf";

    auto results = filter_->Apply(blocks, filter, 10);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].block_id, 101);
}

TEST_F(PostFilterTest, GlobMatch_WildcardStar_MatchesAll) {
    auto blocks = MakeBlocks({101, 201});
    QueryFilter filter;
    filter.source_path = "*";  // Single star matches everything

    auto results = filter_->Apply(blocks, filter, 10);
    ASSERT_EQ(results.size(), 2u);
}

TEST_F(PostFilterTest, GlobMatch_DoubleStar_MatchesAll) {
    auto blocks = MakeBlocks({101, 201});
    QueryFilter filter;
    filter.source_path = "**";  // Double star matches everything

    auto results = filter_->Apply(blocks, filter, 10);
    ASSERT_EQ(results.size(), 2u);
}

TEST_F(PostFilterTest, GlobMatch_EmptyPattern_MatchesAll) {
    auto blocks = MakeBlocks({101, 201});
    QueryFilter filter;
    filter.source_path = "";  // Empty source_path means no filter

    auto results = filter_->Apply(blocks, filter, 10);
    ASSERT_EQ(results.size(), 2u);
}

// --- Combined multi-filter tests ---

TEST_F(PostFilterTest, CombinedFilter_TypePathAndDate) {
    auto blocks = MakeBlocks({101, 102, 201});
    QueryFilter filter;
    filter.block_types = {"FILE"};
    filter.source_path = "/contracts/*";
    filter.created_after = "2024-01-01";

    auto results = filter_->Apply(blocks, filter, 10);
    // Doc 1 created 2024-06-01 -> passes date filter
    // Blocks 101, 102 are FILE in /contracts/* -> pass all filters
    ASSERT_EQ(results.size(), 2u);
    EXPECT_EQ(results[0].block_id, 101);
    EXPECT_EQ(results[1].block_id, 102);
}

TEST_F(PostFilterTest, CombinedFilter_TypeAndDate_ExcludesOldDocs) {
    auto blocks = MakeBlocks({101, 201});
    QueryFilter filter;
    filter.block_types = {"FILE", "DATABASE"};
    filter.created_after = "2025-01-01";

    auto results = filter_->Apply(blocks, filter, 10);
    // Only doc2 (created 2025-01-15) passes date filter; block 201 is DATABASE in doc2
    // Block 101 is in doc1 (created 2024-06-01) -> excluded by date
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].block_id, 201);
}

TEST_F(PostFilterTest, CombinedFilter_AllFiltersExclude) {
    auto blocks = MakeBlocks({101, 102, 201});
    QueryFilter filter;
    filter.block_types = {"DATABASE"};
    filter.source_path = "/contracts/*";
    // DATABASE blocks are in /reports, not /contracts -> nothing matches

    auto results = filter_->Apply(blocks, filter, 10);
    EXPECT_TRUE(results.empty());
}

// --- TopK interaction with filters ---

TEST_F(PostFilterTest, TopK_WithFilter_TruncatesAfterFiltering) {
    auto blocks = MakeBlocks({101, 102, 201});
    QueryFilter filter;
    filter.block_types = {"FILE"};

    // Ask for top_k=1, but 2 FILE blocks pass filter -> only 1 returned
    auto results = filter_->Apply(blocks, filter, 1);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].block_id, 101);
}

// --- Coverage gap: metadata JSON parse error (line 57 catch) ---

TEST_F(PostFilterTest, MetadataJson_Invalid_FallsBackToEmptyObject) {
    // Set doc1 with invalid metadata JSON
    store_.docs["test-doc-1"].metadata_json = "{this is not valid json!!!";
    auto blocks = MakeBlocks({101});
    QueryFilter filter;

    auto results = filter_->Apply(blocks, filter, 10);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].block_id, 101);
    // metadata should be an empty JSON object (catch branch)
    EXPECT_TRUE(results[0].metadata.is_object());
    EXPECT_TRUE(results[0].metadata.empty());
}

TEST_F(PostFilterTest, MetadataJson_Valid_Parsed) {
    // Set doc1 with valid metadata JSON
    store_.docs["test-doc-1"].metadata_json = R"({"key":"value","num":42})";
    auto blocks = MakeBlocks({101});
    QueryFilter filter;

    auto results = filter_->Apply(blocks, filter, 10);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].metadata["key"], "value");
    EXPECT_EQ(results[0].metadata["num"], 42);
}

TEST_F(PostFilterTest, MetadataJson_Empty_EmptyObject) {
    // metadata_json is empty -> skip parse, use empty object
    store_.docs["test-doc-1"].metadata_json = "";
    auto blocks = MakeBlocks({101});
    QueryFilter filter;

    auto results = filter_->Apply(blocks, filter, 10);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_TRUE(results[0].metadata.is_object());
    EXPECT_TRUE(results[0].metadata.empty());
}

// --- Coverage gap: doc_get returns -2 for a block that exists ---
// This happens when a block's doc_id references a deleted doc.

TEST_F(PostFilterTest, BlockExists_DocDeleted_Skipped) {
    // Block 301 exists but references a doc_id that is not in store_
    CortrixBlock blk301;
    blk301.block_id = 301;
    blk301.doc_id = "test-doc-deleted";  // this doc doesn't exist
    blk301.block_type = 1;
    blk301.content_text = "Orphan block";
    store_.blocks[301] = blk301;

    auto blocks = MakeBlocks({301, 101});
    QueryFilter filter;

    auto results = filter_->Apply(blocks, filter, 10);
    // Block 301 should be skipped (doc not found), only block 101 returned
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].block_id, 101);
}

// --- Coverage gap: BlockTypeToString for unknown types ---

TEST_F(PostFilterTest, UnknownBlockType_ToString) {
    CortrixBlock blk;
    blk.block_id = 401;
    blk.doc_id = "test-doc-1";
    blk.block_type = 99;  // Unknown type
    blk.content_text = "Unknown type block";
    store_.blocks[401] = blk;

    auto blocks = MakeBlocks({401});
    QueryFilter filter;

    auto results = filter_->Apply(blocks, filter, 10);
    ASSERT_EQ(results.size(), 1u);
    // Unknown type should produce some string (not crash)
    EXPECT_FALSE(results[0].block_type.empty());
}

// ============================================================
// FetchByDocId tests
// ============================================================

class FetchByDocIdTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Doc 10: image file (SCAN type), 3 chunks
        CortrixDoc doc;
        doc.doc_id = "test-doc-10";
        doc.source_path = "summary_thumbnail.png";
        doc.metadata_json = "";
        store_.docs["test-doc-10"] = doc;

        // Blocks for doc 10 (chunk_index order matters)
        for (int i = 0; i < 3; ++i) {
            CortrixBlock blk;
            blk.block_id  = 1000 + i;
            blk.doc_id    = "test-doc-10";
            blk.chunk_index = i;
            blk.block_type  = 2;  // SCAN
            blk.content_text = "VLM text chunk " + std::to_string(i);
            store_.blocks[1000 + i] = blk;
        }

        // Doc 20: PDF file (FILE type), 2 chunks + 1 empty
        CortrixDoc doc2;
        doc2.doc_id = "test-doc-20";
        doc2.source_path = "report.pdf";
        store_.docs["test-doc-20"] = doc2;

        for (int i = 0; i < 2; ++i) {
            CortrixBlock blk;
            blk.block_id    = 2000 + i;
            blk.doc_id      = "test-doc-20";
            blk.chunk_index = i;
            blk.block_type  = 1;  // FILE
            blk.content_text = "PDF chunk " + std::to_string(i);
            store_.blocks[2000 + i] = blk;
        }
        // Empty content block (should be skipped)
        CortrixBlock empty;
        empty.block_id    = 2002;
        empty.doc_id      = "test-doc-20";
        empty.chunk_index = 2;
        empty.block_type  = 1;
        empty.content_text = "";
        store_.blocks[2002] = empty;

        filter_ = std::make_unique<PostFilter>(store_);
    }

    FakeCortrixStore store_;
    std::unique_ptr<PostFilter> filter_;
};

TEST_F(FetchByDocIdTest, ReturnsAllBlocksForDoc) {
    QueryFilter f;
    auto results = filter_->FetchByDocId("test-doc-10", f, 10);
    ASSERT_EQ(results.size(), 3u);
    for (size_t i = 0; i < 3; ++i) {
        EXPECT_EQ(results[i].block_id, 1000 + static_cast<int>(i));
        EXPECT_EQ(results[i].block_type, "SCAN");
        EXPECT_EQ(results[i].source_path, "summary_thumbnail.png");
        EXPECT_FLOAT_EQ(results[i].score, 1.0f);
        EXPECT_EQ(results[i].hit_routes, kRouteDocId);
    }
}

TEST_F(FetchByDocIdTest, RespectsTopK) {
    QueryFilter f;
    auto results = filter_->FetchByDocId("test-doc-10", f, 2);
    ASSERT_EQ(results.size(), 2u);
    EXPECT_EQ(results[0].block_id, 1000);
    EXPECT_EQ(results[1].block_id, 1001);
}

TEST_F(FetchByDocIdTest, SkipsEmptyContentBlocks) {
    QueryFilter f;
    auto results = filter_->FetchByDocId("test-doc-20", f, 10);
    // Only 2 non-empty blocks returned (empty block 2002 is skipped)
    ASSERT_EQ(results.size(), 2u);
    EXPECT_EQ(results[0].block_id, 2000);
    EXPECT_EQ(results[1].block_id, 2001);
}

TEST_F(FetchByDocIdTest, ExcludeTypesFilter_FiltersMemory) {
    // Add a MEMORY block to doc 10
    CortrixBlock mem;
    mem.block_id    = 1099;
    mem.doc_id      = "test-doc-10";
    mem.chunk_index = 99;
    mem.block_type  = 7;  // MEMORY
    mem.content_text = "stale memory content";
    store_.blocks[1099] = mem;

    QueryFilter f;
    f.exclude_types = {"MEMORY"};
    auto results = filter_->FetchByDocId("test-doc-10", f, 10);
    // 3 SCAN + 1 MEMORY, MEMORY excluded -> 3 results
    ASSERT_EQ(results.size(), 3u);
    for (auto& r : results) {
        EXPECT_NE(r.block_type, "MEMORY");
    }
}

TEST_F(FetchByDocIdTest, UnknownDoc_ReturnsEmpty) {
    QueryFilter f;
    auto results = filter_->FetchByDocId("test-doc-unknown", f, 10);
    // this doc doesn't exist; block_get_by_doc returns empty; doc_get fails
    // but we still return empty results gracefully
    EXPECT_TRUE(results.empty());
}

TEST_F(FetchByDocIdTest, ChunksReturnedInOrder) {
    QueryFilter f;
    auto results = filter_->FetchByDocId("test-doc-10", f, 10);
    ASSERT_EQ(results.size(), 3u);
    // chunk_index 0, 1, 2 in order
    EXPECT_EQ(results[0].chunk_text, "VLM text chunk 0");
    EXPECT_EQ(results[1].chunk_text, "VLM text chunk 1");
    EXPECT_EQ(results[2].chunk_text, "VLM text chunk 2");
}

}  // namespace
}  // namespace cortrix
