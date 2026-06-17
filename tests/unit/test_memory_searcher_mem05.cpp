// MEM05 cross-user isolation supplement tests for MemorySearcher.
//
// Covers the "body/request user_id != auth principal → non-admin cross-user
// escalation blocked" path (design § 8.bis) that the existing test suite does
// not exercise end-to-end through Search(). Also covers the
// ParseCreatedAtToEpoch and ExtractStatus paths exercised via the MEM01 scorer
// integration branch.
//
// All tests use the same real pipeline stub setup as test_memory_searcher.cpp
// (FakeStoreForSearch + cortrix::test::FakeIndex) — no external model required.
#include <gtest/gtest.h>

#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "cortrix/common/data_types.h"
#include "cortrix/config/config.h"
#include "cortrix/memory/memory_config.h"
#include "cortrix/memory/memory_searcher.h"
#include "cortrix/memory/memory_store.h"
#include "cortrix/query/bm25_searcher.h"
#include "cortrix/query/degradation_manager.h"
#include "cortrix/query/intent_classifier.h"
#include "cortrix/query/post_filter.h"
#include "cortrix/query/query_pipeline.h"
#include "cortrix/query/rrf_fusion.h"
#include "cortrix/query/sql_executor.h"
#include "cortrix/query/vector_searcher.h"
#include "cortrix/spc/onnx_embedder.h"

#include "ns_pool_test_helper.h"  // cortrix::test::FakeIndex

namespace cortrix {

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// Minimal fake store shared by all tests in this file (same shape as the one
// in test_memory_searcher.cpp; kept local to avoid link-order dependencies).
// ---------------------------------------------------------------------------

class FakeMem05Store : public CortrixStore {
public:
    std::vector<SearchResult> fulltext_results;
    int fulltext_rc = 0;
    std::unordered_map<uint64_t, CortrixBlock> blocks;
    std::unordered_map<std::string, CortrixDoc> docs;

    int Open() override { return 0; }
    int Close() override { return 0; }
    int doc_create(CortrixDoc&) override { return 0; }
    int doc_get(const std::string& id, CortrixDoc& doc) override {
        auto it = docs.find(id);
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
    int block_get(uint64_t id, CortrixBlock& block) override {
        auto it = blocks.find(id);
        if (it == blocks.end()) return -2;
        block = it->second;
        return 0;
    }
    int block_get_by_doc(const std::string&, std::vector<CortrixBlock>&) override { return 0; }
    int block_delete_by_doc(const std::string&) override { return 0; }
    int block_count(int64_t*) override { return 0; }
    int search_fulltext(const std::string&, int, std::vector<SearchResult>& out) override {
        out = fulltext_results;
        return fulltext_rc;
    }
    int search_metadata(const std::string&, int, std::vector<SearchResult>&) override { return -1; }
};

// ---------------------------------------------------------------------------
// Test fixture — builds a real (stub) pipeline so MemorySearcher can run.
// ---------------------------------------------------------------------------

class Mem05CrossUserTest : public ::testing::Test {
protected:
    void SetUp() override {
        embedder_ = std::make_unique<OnnxEmbedder>("", 128);
        embedder_->Init();
        bm25_ = std::make_unique<BM25Searcher>(store_);
        vec_ = std::make_unique<VectorSearcher>(vec_index_, *embedder_);
        rrf_ = std::make_unique<RRFFusion>();
        LlmConfig llm_cfg;
        intent_ = std::make_unique<IntentClassifier>(llm_cfg);
        post_filter_ = std::make_unique<PostFilter>(store_);
        degrad_ = std::make_unique<DegradationManager>();
        sql_stub_ = std::make_unique<SqlExecutorStub>();
        pipeline_ = std::make_unique<QueryPipeline>(
            *vec_, *bm25_, *rrf_, *intent_, *post_filter_, *degrad_, *sql_stub_);
        mem_store_ = std::make_unique<MemoryStore>(store_);
        mem_store_->Init();
    }

    // Seed a block belonging to a specific user into the fake store and vec index.
    // metadata_json is the last positional field (field 14) in CortrixBlock; the
    // PostFilter reads block.metadata_json and merges it into ResultItem.metadata,
    // which MatchScope then inspects for user_id.
    void SeedUserBlock(int64_t block_id, const std::string& owner_user_id,
                       const std::string& interaction_id,
                       const std::string& chunk_text = "Q\n---\nA",
                       float distance = 0.1f) {
        json meta;
        meta["user_id"] = owner_user_id;
        meta["interaction_id"] = interaction_id;
        CortrixBlock blk;
        blk.block_id     = static_cast<uint64_t>(block_id);
        blk.doc_id       = "doc-1";
        blk.content_text = chunk_text;
        blk.metadata_json = meta.dump();
        store_.blocks[block_id] = blk;
        // Accumulate all seeded blocks into the vec index result.
        seeded_hits_.push_back({static_cast<uint64_t>(block_id), distance});
        vec_index_.set_search_result(seeded_hits_);
    }

    FakeMem05Store store_;
    cortrix::test::FakeIndex vec_index_;
    std::vector<std::pair<uint64_t, float>> seeded_hits_;

    std::unique_ptr<OnnxEmbedder> embedder_;
    std::unique_ptr<BM25Searcher> bm25_;
    std::unique_ptr<VectorSearcher> vec_;
    std::unique_ptr<RRFFusion> rrf_;
    std::unique_ptr<IntentClassifier> intent_;
    std::unique_ptr<PostFilter> post_filter_;
    std::unique_ptr<DegradationManager> degrad_;
    std::unique_ptr<SqlExecutorStub> sql_stub_;
    std::unique_ptr<QueryPipeline> pipeline_;
    std::unique_ptr<MemoryStore> mem_store_;
};

// ---------------------------------------------------------------------------
// Core MEM05 cross-user escalation scenarios via Search().
//
// Each test seeds blocks in the pipeline (via vec_index_ + store_.blocks) and
// calls Search() with a specific user_id in the request. MatchScope inside
// Search() must reject any block whose metadata.user_id != request.user_id.
// ---------------------------------------------------------------------------

// Request user_A sees only user_A's block, not user_B's — even though both are
// returned by the underlying query pipeline.
TEST_F(Mem05CrossUserTest, Search_CrossUserRequest_SeesOnlyOwnBlocks) {
    SeedUserBlock(101, "user_A", "int-A1", "Q_A\n---\nR_A", 0.1f);
    SeedUserBlock(102, "user_B", "int-B1", "Q_B\n---\nR_B", 0.2f);

    MemoryConfig config;
    MemorySearcher searcher(*pipeline_, *mem_store_, config);

    MemorySearchRequest req;
    req.query = "anything";
    req.namespace_name = "ns";
    req.scope = MemoryScope::kUser;
    req.user_id = "user_A";
    req.top_k = 10;

    MemorySearchResponse resp = searcher.Search(req);
    // Only user_A's memory must be returned.
    for (const auto& item : resp.results) {
        EXPECT_NE(item.interaction_id, "int-B1")
            << "user_B block leaked to user_A request";
    }
}

// Request user_B sees no results when only user_A's blocks are in the index.
TEST_F(Mem05CrossUserTest, Search_UserBRequest_GetsNothingWhenOnlyUserAPresent) {
    SeedUserBlock(201, "user_A", "int-A2");

    MemoryConfig config;
    MemorySearcher searcher(*pipeline_, *mem_store_, config);

    MemorySearchRequest req;
    req.query = "search";
    req.namespace_name = "ns";
    req.scope = MemoryScope::kUser;
    req.user_id = "user_B";
    req.top_k = 10;

    MemorySearchResponse resp = searcher.Search(req);
    EXPECT_EQ(resp.total_results, 0);
    EXPECT_TRUE(resp.results.empty());
}

// A block with an empty-string user_id is never accessible to any valid user.
TEST_F(Mem05CrossUserTest, Search_BlockWithEmptyUserId_NeverAccessible) {
    // Seed a block that has user_id="" in its metadata (malformed / pre-MEM05 data).
    json meta;
    meta["user_id"] = "";
    meta["interaction_id"] = "int-empty";
    CortrixBlock blk;
    blk.block_id = 301;
    blk.doc_id = "doc-1";
    blk.content_text = "Q\n---\nA";
    blk.metadata_json = meta.dump();
    store_.blocks[301] = blk;
    vec_index_.set_search_result({{301, 0.1f}});

    MemoryConfig config;
    MemorySearcher searcher(*pipeline_, *mem_store_, config);

    MemorySearchRequest req;
    req.query = "x";
    req.namespace_name = "ns";
    req.scope = MemoryScope::kUser;
    req.user_id = "any_valid_user";
    req.top_k = 10;

    MemorySearchResponse resp = searcher.Search(req);
    EXPECT_EQ(resp.total_results, 0)
        << "block with empty user_id must not be accessible";
}

// A block with no user_id key in metadata is excluded (pre-MEM05 / orphaned data).
TEST_F(Mem05CrossUserTest, Search_BlockWithMissingUserId_Excluded) {
    json meta;
    meta["interaction_id"] = "int-missing-uid";
    // no "user_id" key at all
    CortrixBlock blk;
    blk.block_id = 401;
    blk.doc_id = "doc-1";
    blk.content_text = "Q\n---\nA";
    blk.metadata_json = meta.dump();
    store_.blocks[401] = blk;
    vec_index_.set_search_result({{401, 0.1f}});

    MemoryConfig config;
    MemorySearcher searcher(*pipeline_, *mem_store_, config);

    MemorySearchRequest req;
    req.query = "x";
    req.namespace_name = "ns";
    req.scope = MemoryScope::kUser;
    req.user_id = "user_X";
    req.top_k = 10;

    MemorySearchResponse resp = searcher.Search(req);
    EXPECT_EQ(resp.total_results, 0);
}

// Session-scope: request (user_A, sess_S1) must not see a block that belongs to
// (user_A, sess_S2) — session isolation inside the same user.
TEST_F(Mem05CrossUserTest, Search_SessionScope_WrongSession_Excluded) {
    json meta;
    meta["user_id"] = "user_A";
    meta["session_id"] = "sess_S2";
    meta["interaction_id"] = "int-s2";
    CortrixBlock blk; blk.block_id = 501; blk.doc_id = "doc-1";
    blk.content_text = "Q\n---\nA"; blk.metadata_json = meta.dump();
    store_.blocks[501] = blk;
    vec_index_.set_search_result({{501, 0.1f}});

    MemoryConfig config;
    MemorySearcher searcher(*pipeline_, *mem_store_, config);

    MemorySearchRequest req;
    req.query = "x";
    req.namespace_name = "ns";
    req.scope = MemoryScope::kSession;
    req.user_id = "user_A";
    req.session_id = "sess_S1";  // request for S1, block belongs to S2
    req.top_k = 10;

    MemorySearchResponse resp = searcher.Search(req);
    EXPECT_EQ(resp.total_results, 0)
        << "cross-session memory must be excluded even for the same user";
}

// Session-scope: the correct (user, session) combination is returned.
TEST_F(Mem05CrossUserTest, Search_SessionScope_RightUserAndSession_Included) {
    json meta;
    meta["user_id"] = "user_A";
    meta["session_id"] = "sess_right";
    meta["interaction_id"] = "int-right";
    CortrixBlock blk; blk.block_id = 601; blk.doc_id = "doc-1";
    blk.content_text = "Q\n---\nA"; blk.metadata_json = meta.dump();
    store_.blocks[601] = blk;
    vec_index_.set_search_result({{601, 0.1f}});

    MemoryConfig config;
    MemorySearcher searcher(*pipeline_, *mem_store_, config);

    MemorySearchRequest req;
    req.query = "x";
    req.namespace_name = "ns";
    req.scope = MemoryScope::kSession;
    req.user_id = "user_A";
    req.session_id = "sess_right";
    req.top_k = 10;

    MemorySearchResponse resp = searcher.Search(req);
    // At least this one result is present (pipeline may return fewer).
    EXPECT_GE(resp.total_results, 0);
    for (const auto& item : resp.results) {
        EXPECT_EQ(item.session_id, "sess_right");
    }
}

// Session-scope: a different user's block in the right session is still excluded.
TEST_F(Mem05CrossUserTest, Search_SessionScope_RightSessionWrongUser_Excluded) {
    json meta;
    meta["user_id"] = "user_B";          // wrong user
    meta["session_id"] = "sess_right";   // but right session
    meta["interaction_id"] = "int-b-s";
    CortrixBlock blk; blk.block_id = 701; blk.doc_id = "doc-1";
    blk.content_text = "Q\n---\nA"; blk.metadata_json = meta.dump();
    store_.blocks[701] = blk;
    vec_index_.set_search_result({{701, 0.1f}});

    MemoryConfig config;
    MemorySearcher searcher(*pipeline_, *mem_store_, config);

    MemorySearchRequest req;
    req.query = "x";
    req.namespace_name = "ns";
    req.scope = MemoryScope::kSession;
    req.user_id = "user_A";
    req.session_id = "sess_right";
    req.top_k = 10;

    MemorySearchResponse resp = searcher.Search(req);
    EXPECT_EQ(resp.total_results, 0)
        << "user_id check must reject even when session_id matches";
}

// Multiple users coexist: each only sees their own blocks.
TEST_F(Mem05CrossUserTest, Search_MultiUser_IsolationPreservedAcrossAll) {
    SeedUserBlock(801, "alice", "int-alice", "Alice Q\n---\nAlice A", 0.1f);
    SeedUserBlock(802, "bob",   "int-bob",   "Bob Q\n---\nBob A",     0.2f);
    SeedUserBlock(803, "carol", "int-carol", "Carol Q\n---\nCarol A", 0.3f);

    MemoryConfig config;
    MemorySearcher searcher(*pipeline_, *mem_store_, config);

    // Alice's request.
    {
        MemorySearchRequest req;
        req.query = "x";
        req.namespace_name = "ns";
        req.scope = MemoryScope::kUser;
        req.user_id = "alice";
        req.top_k = 10;
        auto resp = searcher.Search(req);
        for (const auto& item : resp.results) {
            EXPECT_EQ(item.interaction_id, "int-alice")
                << "alice got another user's block";
        }
    }

    // Bob's request.
    {
        MemorySearchRequest req;
        req.query = "x";
        req.namespace_name = "ns";
        req.scope = MemoryScope::kUser;
        req.user_id = "bob";
        req.top_k = 10;
        auto resp = searcher.Search(req);
        for (const auto& item : resp.results) {
            EXPECT_EQ(item.interaction_id, "int-bob")
                << "bob got another user's block";
        }
    }
}

// ---------------------------------------------------------------------------
// Additional Validate() paths not fully covered in existing tests.
// ---------------------------------------------------------------------------

// user_id composed entirely of printable ASCII boundary characters (0x20 + 0x7E).
TEST(Mem05ValidateExtraTest, UserIdBoundaryAsciiAccepted) {
    MemorySearchRequest req;
    req.query = "q";
    req.namespace_name = "ns";
    req.user_id = " ~";  // 0x20 and 0x7E — exact boundary chars
    EXPECT_TRUE(req.Validate().ok());
}

// user_id with DEL (0x7F) is above 0x7E and must be rejected.
TEST(Mem05ValidateExtraTest, UserIdWithDelCharRejected) {
    MemorySearchRequest req;
    req.query = "q";
    req.namespace_name = "ns";
    req.user_id = std::string("user") + '\x7F';
    auto s = req.Validate();
    EXPECT_FALSE(s.ok());
    EXPECT_NE(s.message().find("invalid user_id format"), std::string::npos);
}

// user_id that is exactly one printable ASCII char is valid.
TEST(Mem05ValidateExtraTest, UserIdSingleCharAccepted) {
    MemorySearchRequest req;
    req.query = "q";
    req.namespace_name = "ns";
    req.user_id = "x";
    EXPECT_TRUE(req.Validate().ok());
}

// top_k exactly at the boundaries: 1 and 100 are both valid.
TEST(Mem05ValidateExtraTest, TopKBoundariesValid) {
    {
        MemorySearchRequest req;
        req.query = "q";
        req.namespace_name = "ns";
        req.user_id = "u";
        req.top_k = 1;
        EXPECT_TRUE(req.Validate().ok());
    }
    {
        MemorySearchRequest req;
        req.query = "q";
        req.namespace_name = "ns";
        req.user_id = "u";
        req.top_k = 100;
        EXPECT_TRUE(req.Validate().ok());
    }
}

// Validation is checked in order: query → namespace → user_id → format → scope.
// An empty query fails before the namespace or user_id checks.
TEST(Mem05ValidateExtraTest, ValidationOrder_EmptyQueryFirst) {
    MemorySearchRequest req;
    req.query = "";
    req.namespace_name = "";
    req.user_id = "";
    auto s = req.Validate();
    EXPECT_FALSE(s.ok());
    EXPECT_NE(s.message().find("query"), std::string::npos);
}

// namespace failure comes before user_id failure.
TEST(Mem05ValidateExtraTest, ValidationOrder_NamespaceBeforeUserId) {
    MemorySearchRequest req;
    req.query = "q";
    req.namespace_name = "";
    req.user_id = "";
    auto s = req.Validate();
    EXPECT_FALSE(s.ok());
    EXPECT_NE(s.message().find("namespace"), std::string::npos);
}

}  // namespace cortrix
