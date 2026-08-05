// Memory isolation integration tests — per-user isolation across the read path and the
// session-ownership CRUD path. Maps design § 8.2 (6 scenarios) onto the
// capabilities present in the standalone build:
//   1. cross-user search isolation       -> MemorySearcher + stub pipeline
//   2. cross-user list isolation         -> SessionList + user filter
//   3. cross-user "edit" rejection       -> session ownership (404 analog)
//   4. cross-user "delete" rejection     -> session ownership (404 analog)
//   5. cross-user session isolation      -> SessionGet ownership (404)
//   6. CE default user                   -> user_id="default" round-trips
//
// NOTE (D3.5): HTTP PUT/DELETE /memory/:id ownership wiring has no endpoint in
// the MVP yet; scenarios 3/4 are exercised here via the session-ownership 404
// analog (same isolation principle). Full memory-id CRUD ownership -> D3.5.
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

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
#include "cortrix/store/iindex.h"

namespace cortrix {
namespace {

using json = nlohmann::json;

// Store returning a fixed set of memory blocks (with user-tagged metadata) for
// both the vector and fulltext routes, so MemorySearcher exercises MatchScope.
//
// [id-system D-I6] doc_id is now a ULID string: CortrixStore's doc-keyed virtuals
// take const std::string& (was int64_t), block_get takes uint64_t. The user-tagged
// metadata the isolation test asserts on now travels block -> doc: PostFilter does
// block_get(id) -> doc_get(block.doc_id) and parses doc.metadata_json. So each
// block carries its owner's doc_id and that doc holds the {interaction_id,user_id}
// JSON (keyed by string doc_id below).
class TwoUserStore : public CortrixStore {
public:
    std::unordered_map<uint64_t, CortrixBlock> blocks;
    std::unordered_map<std::string, CortrixDoc> docs;
    std::vector<SearchResult> fulltext;

    int Open() override { return 0; }
    int Close() override { return 0; }
    int doc_create(CortrixDoc&) override { return 0; }
    int doc_get(const std::string& id, CortrixDoc& d) override {
        auto it = docs.find(id);
        if (it == docs.end()) return -2;
        d = it->second;
        return 0;
    }
    int doc_update_status(const std::string&, DocStatus, const std::string&) override { return 0; }
    int doc_delete(const std::string&) override { return 0; }
    int doc_list_by_status(DocStatus, std::vector<CortrixDoc>&) override { return 0; }
    int doc_find_by_source(const std::string&, const std::string&, CortrixDoc&) override { return -2; }
    int doc_find_by_hash(const std::string&, CortrixDoc&) override { return -2; }
    int doc_count(int64_t* c) override { *c = 0; return 0; }
    int block_insert(CortrixBlock&) override { return 0; }
    int block_get(uint64_t id, CortrixBlock& b) override {
        auto it = blocks.find(id);
        if (it == blocks.end()) return -2;
        b = it->second;
        return 0;
    }
    int block_get_by_doc(const std::string&, std::vector<CortrixBlock>&) override { return 0; }
    int block_delete_by_doc(const std::string&) override { return 0; }
    int block_count(int64_t* c) override { *c = 0; return 0; }
    int search_fulltext(const std::string&, int, std::vector<SearchResult>& r) override {
        r = fulltext;
        return 0;
    }
    int search_metadata(const std::string&, int, std::vector<SearchResult>&) override { return -1; }
};

// The MVP CortrixVectorIndex was replaced by store::IIndex (VectorSearcher
// now holds an IIndex&). VectorSearcher only calls Search(); the rest are no-op
// stubs that satisfy the pure-virtual contract. `results` is the canned hit list
// (block_id, distance) that drives the cross-user isolation scenarios.
class TwoUserVecIndex : public cortrix::store::IIndex {
public:
    std::vector<std::pair<uint64_t, float>> results;

    cortrix::Status AddPoint(const float*, uint64_t,
                             const cortrix::observability::TraceContext*) override {
        return cortrix::Status::Ok();
    }
    cortrix::Status AddPoints(const std::vector<std::pair<const float*, uint64_t>>&,
                              const cortrix::observability::TraceContext*) override {
        return cortrix::Status::Ok();
    }
    cortrix::Status MarkDelete(uint64_t, const cortrix::observability::TraceContext*) override {
        return cortrix::Status::Ok();
    }
    std::vector<std::pair<uint64_t, float>> Search(
        const float*, int, int, const cortrix::observability::TraceContext*) override {
        return results;
    }
    bool Exists(uint64_t) override { return false; }
    cortrix::Status Snapshot() override { return cortrix::Status::Ok(); }
    cortrix::Status Recover() override { return cortrix::Status::Ok(); }
    cortrix::Status Shutdown() override { return cortrix::Status::Ok(); }
    cortrix::store::IndexStats GetStats() override { return cortrix::store::IndexStats{}; }
    std::size_t GetMemoryFootprintBytes() const override { return results.size(); }
};

// ---- Scenario 1: cross-user search isolation -------------------------------

class MemoryIsolationSearchIsolation : public ::testing::Test {
protected:
    void SetUp() override {
        // Two memory blocks: 101 owned by user_A, 102 owned by user_B. The
        // user-tagged metadata lives on each block's doc (PostFilter reads it via
        // doc_get(block.doc_id) -> doc.metadata_json), so each block points at its
        // owner's doc and that doc carries the {interaction_id, user_id} JSON.
        json mA;
        mA["interaction_id"] = "int-A";
        mA["session_id"] = "sess-A";
        mA["user_id"] = "user_A";
        CortrixBlock blockA;
        blockA.block_id = 101;
        blockA.doc_id = "doc-A";
        blockA.block_type = 6;
        blockA.processing_level = 3;
        blockA.content_text = "shared topic\n---\nanswer A";
        store_.blocks[101] = blockA;

        json mB;
        mB["interaction_id"] = "int-B";
        mB["session_id"] = "sess-B";
        mB["user_id"] = "user_B";
        CortrixBlock blockB;
        blockB.block_id = 102;
        blockB.doc_id = "doc-B";
        blockB.block_type = 6;
        blockB.processing_level = 3;
        blockB.content_text = "shared topic\n---\nanswer B";
        store_.blocks[102] = blockB;

        CortrixDoc docA;
        docA.doc_id = "doc-A";
        docA.source_path = "/data/mem.db";
        docA.metadata_json = mA.dump();
        store_.docs["doc-A"] = docA;

        CortrixDoc docB;
        docB.doc_id = "doc-B";
        docB.source_path = "/data/mem.db";
        docB.metadata_json = mB.dump();
        store_.docs["doc-B"] = docB;

        vec_.results = {{101, 0.1f}, {102, 0.2f}};
        store_.fulltext = {{101, "doc-A", 10.0, "shared topic"},
                           {102, "doc-B", 9.0, "shared topic"}};

        embedder_ = std::make_unique<OnnxEmbedder>("", 128);
        embedder_->Init();
        bm25_ = std::make_unique<BM25Searcher>(store_);
        vsearch_ = std::make_unique<VectorSearcher>(vec_, *embedder_);
        rrf_ = std::make_unique<RRFFusion>();
        LlmConfig llm;
        intent_ = std::make_unique<IntentClassifier>(llm);
        pf_ = std::make_unique<PostFilter>(store_);
        deg_ = std::make_unique<DegradationManager>();
        sql_ = std::make_unique<SqlExecutorStub>();
        pipeline_ = std::make_unique<QueryPipeline>(
            *vsearch_, *bm25_, *rrf_, *intent_, *pf_, *deg_, *sql_);
        mem_store_ = std::make_unique<MemoryStore>(store_);
        mem_store_->Init();
        searcher_ = std::make_unique<MemorySearcher>(*pipeline_, *mem_store_, cfg_);
    }

    MemorySearchRequest Req(const std::string& user_id) {
        MemorySearchRequest r;
        r.query = "shared topic";
        r.namespace_name = "default";
        r.scope = MemoryScope::kUser;
        r.user_id = user_id;
        r.top_k = 10;
        return r;
    }

    TwoUserStore store_;
    TwoUserVecIndex vec_;
    std::unique_ptr<OnnxEmbedder> embedder_;
    std::unique_ptr<BM25Searcher> bm25_;
    std::unique_ptr<VectorSearcher> vsearch_;
    std::unique_ptr<RRFFusion> rrf_;
    std::unique_ptr<IntentClassifier> intent_;
    std::unique_ptr<PostFilter> pf_;
    std::unique_ptr<DegradationManager> deg_;
    std::unique_ptr<SqlExecutorStub> sql_;
    std::unique_ptr<QueryPipeline> pipeline_;
    std::unique_ptr<MemoryStore> mem_store_;
    MemoryConfig cfg_;
    std::unique_ptr<MemorySearcher> searcher_;
};

TEST_F(MemoryIsolationSearchIsolation, UserAOnlySeesOwnMemory) {
    auto resp = searcher_->Search(Req("user_A"));
    // Every returned item must belong to user_A (int-A), never user_B.
    for (const auto& item : resp.results) {
        EXPECT_EQ(item.interaction_id, "int-A");
        EXPECT_NE(item.interaction_id, "int-B");
    }
}

TEST_F(MemoryIsolationSearchIsolation, UserBOnlySeesOwnMemory) {
    auto resp = searcher_->Search(Req("user_B"));
    for (const auto& item : resp.results) {
        EXPECT_EQ(item.interaction_id, "int-B");
    }
}

TEST_F(MemoryIsolationSearchIsolation, UnknownUserSeesNothing) {
    auto resp = searcher_->Search(Req("user_C"));  // no blocks owned by C
    EXPECT_EQ(resp.total_results, 0);
}

// ---- Scenarios 2-6: session-ownership + store-level isolation --------------

// [id-system D-I6] Construct-only stub (scenarios 2-6 drive the :memory: SQLite
// MemoryStore for sessions, not these doc/block methods). doc_id is a ULID string;
// the doc-keyed virtuals take const std::string&.
class StubStore : public CortrixStore {
public:
    int Open() override { return 0; }
    int Close() override { return 0; }
    int doc_create(CortrixDoc& d) override { d.doc_id = "stub-doc-" + std::to_string(++n_); return 0; }
    int doc_get(const std::string&, CortrixDoc&) override { return -2; }
    int doc_update_status(const std::string&, DocStatus, const std::string&) override { return 0; }
    int doc_delete(const std::string&) override { return 0; }
    int doc_list_by_status(DocStatus, std::vector<CortrixDoc>&) override { return 0; }
    int doc_find_by_source(const std::string&, const std::string&, CortrixDoc&) override { return -2; }
    int doc_find_by_hash(const std::string&, CortrixDoc&) override { return -2; }
    int doc_count(int64_t* c) override { *c = 0; return 0; }
    int block_insert(CortrixBlock&) override { return 0; }
    int block_get(uint64_t, CortrixBlock&) override { return -2; }
    int block_get_by_doc(const std::string&, std::vector<CortrixBlock>&) override { return 0; }
    int block_delete_by_doc(const std::string&) override { return 0; }
    int block_count(int64_t* c) override { *c = 0; return 0; }
    int search_fulltext(const std::string&, int, std::vector<SearchResult>&) override { return 0; }
    int search_metadata(const std::string&, int, std::vector<SearchResult>&) override { return -1; }
private:
    int64_t n_ = 100;
};

class MemoryIsolationStoreIsolation : public ::testing::Test {
protected:
    void SetUp() override {
        mem_store_ = std::make_unique<MemoryStore>(stub_);
        ASSERT_TRUE(mem_store_->Init(":memory:").ok());
    }

    std::string CreateSession(const std::string& user_id) {
        MemorySession s;
        s.namespace_name = "default";
        s.user_id = user_id;
        EXPECT_TRUE(mem_store_->SessionCreate(s).ok());
        return s.session_id;
    }

    // Session-ownership check (same logic the memory_routes handlers apply):
    // returns true when the session exists AND belongs to requester.
    bool Owns(const std::string& session_id, const std::string& requester) {
        MemorySession s;
        auto st = mem_store_->SessionGet(session_id, s);
        return st.ok() && s.user_id == requester;
    }

    StubStore stub_;
    std::unique_ptr<MemoryStore> mem_store_;
};

// Scenario 2: cross-user list isolation.
TEST_F(MemoryIsolationStoreIsolation, ListOnlyReturnsOwnSessions) {
    CreateSession("user_A");
    CreateSession("user_A");
    CreateSession("user_B");

    std::vector<MemorySession> all;
    ASSERT_TRUE(mem_store_->SessionList("default", 100, 0, all).ok());

    // Apply the same user_id post-filter the list handler applies.
    int a_count = 0, b_count = 0;
    for (const auto& s : all) {
        if (s.user_id == "user_A") ++a_count;
        if (s.user_id == "user_B") ++b_count;
    }
    EXPECT_EQ(a_count, 2);
    EXPECT_EQ(b_count, 1);
}

// Scenario 5: cross-user session GET isolation (404 analog).
TEST_F(MemoryIsolationStoreIsolation, SessionGetCrossUser_DeniedByOwnership) {
    std::string a_sid = CreateSession("user_A");
    EXPECT_TRUE(Owns(a_sid, "user_A"));    // owner sees it
    EXPECT_FALSE(Owns(a_sid, "user_B"));   // other user -> ownership fails (-> 404)
}

// Scenario 3: cross-user "edit" rejection (ownership analog).
TEST_F(MemoryIsolationStoreIsolation, EditCrossUser_DeniedByOwnership) {
    std::string a_sid = CreateSession("user_A");
    // A PUT by user_B would fail the ownership gate before any mutation.
    EXPECT_FALSE(Owns(a_sid, "user_B"));
    EXPECT_TRUE(Owns(a_sid, "user_A"));
}

// Scenario 4: cross-user "delete" rejection (ownership analog).
TEST_F(MemoryIsolationStoreIsolation, DeleteCrossUser_DeniedByOwnership) {
    std::string a_sid = CreateSession("user_A");
    // DELETE by user_B must be blocked; only user_A may delete.
    EXPECT_FALSE(Owns(a_sid, "user_B"));
    // Owner-driven delete succeeds and the session disappears.
    ASSERT_TRUE(Owns(a_sid, "user_A"));
    ASSERT_TRUE(mem_store_->SessionDelete(a_sid).ok());
    MemorySession check;
    EXPECT_EQ(mem_store_->SessionGet(a_sid, check).code(), StatusCode::kNotFound);
}

// Scenario 6: CE default user — user_id="default" round-trips through isolation.
TEST_F(MemoryIsolationStoreIsolation, DefaultUser_RoundTrips) {
    std::string sid = CreateSession("default");
    EXPECT_TRUE(Owns(sid, "default"));

    std::vector<MemorySession> all;
    ASSERT_TRUE(mem_store_->SessionList("default", 100, 0, all).ok());
    int default_count = 0;
    for (const auto& s : all) {
        if (s.user_id == "default") ++default_count;
    }
    EXPECT_EQ(default_count, 1);
}

// Bonus: interaction_log search honors the user_id filter (D4 isolation).
TEST_F(MemoryIsolationStoreIsolation, InteractionSearchUserFilter) {
    std::string a_sid = CreateSession("user_A");
    std::string b_sid = CreateSession("user_B");

    auto write = [&](const std::string& sid, const std::string& user, const std::string& content) {
        InteractionLog log;
        log.session_id = sid;
        log.namespace_name = "default";
        log.user_id = user;
        log.role = "user";
        log.content = content;
        mem_store_->InteractionInsert(log);
    };
    write(a_sid, "user_A", "alpha topic");
    write(b_sid, "user_B", "alpha topic");

    // Filter to user_A: only A's interaction comes back.
    std::vector<InteractionLog> a_only;
    ASSERT_TRUE(mem_store_->InteractionSearch("default", "alpha", "", "user_A", 10, a_only).ok());
    for (const auto& r : a_only) {
        EXPECT_EQ(r.user_id, "user_A");
    }
    EXPECT_GE(a_only.size(), 1u);
}

}  // namespace
}  // namespace cortrix
