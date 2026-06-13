// MEM05 unit tests — Per-user memory isolation.
// Covers design § 8.1 matrix (14 cases) + user_id format + a few extras:
//   - MemorySearchRequest::Validate() : user_id unconditionally required,
//     format enforced (<=128 chars, ASCII printable).
//   - MemorySearcher::MatchScope()   : every path filters user_id first.
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

#include "ns_pool_test_helper.h"  // cortrix::test::FakeIndex (IIndex fake)

namespace cortrix {

// Test fixture (friend of MemorySearcher) — builds a real stub pipeline so we
// can construct a MemorySearcher and reach the private MatchScope().
class Mem05IsolationTest : public ::testing::Test {
protected:
    class FakeStore : public CortrixStore {
    public:
        int Open() override { return 0; }
        int Close() override { return 0; }
        int doc_create(CortrixDoc&) override { return 0; }
        int doc_get(const std::string&, CortrixDoc&) override { return -2; }
        int doc_update_status(const std::string&, DocStatus, const std::string&) override { return 0; }
        int doc_delete(const std::string&) override { return 0; }
        int doc_list_by_status(DocStatus, std::vector<CortrixDoc>&) override { return 0; }
        int doc_find_by_source(const std::string&, const std::string&, CortrixDoc&) override { return -2; }
        int doc_find_by_hash(const std::string&, CortrixDoc&) override { return -2; }
        int doc_count(int64_t*) override { return 0; }
        int block_insert(CortrixBlock&) override { return 0; }
        int block_get(uint64_t, CortrixBlock&) override { return -2; }
        int block_get_by_doc(const std::string&, std::vector<CortrixBlock>&) override { return 0; }
        int block_delete_by_doc(const std::string&) override { return 0; }
        int block_count(int64_t*) override { return 0; }
        int search_fulltext(const std::string&, int, std::vector<SearchResult>&) override { return 0; }
        int search_metadata(const std::string&, int, std::vector<SearchResult>&) override { return -1; }
    };
    // VectorSearcher now takes a store::IIndex& (F05 wire⑤c); the shared
    // cortrix::test::FakeIndex stands in for the old CortrixVectorIndex fake.
    // These tests only need the pipeline to construct (they exercise
    // MatchScope/Validate), so the default empty Search result is fine.

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
        searcher_ = std::make_unique<MemorySearcher>(*pipeline_, *mem_store_, config_);
    }

    bool MatchScope(const nlohmann::json& meta, const MemorySearchRequest& req) {
        return searcher_->MatchScope(meta, req);
    }

    FakeStore store_;
    cortrix::test::FakeIndex vec_index_;
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
    MemoryConfig config_;
    std::unique_ptr<MemorySearcher> searcher_;
};

namespace {
using json = nlohmann::json;

MemorySearchRequest BaseReq() {
    MemorySearchRequest r;
    r.query = "q";
    r.namespace_name = "ns";
    return r;
}

// ===== § 8.1 case 1-4, 12-13: Validate() =====

// 1. user_id empty -> InvalidArgument
TEST_F(Mem05IsolationTest, Validate_UserIdEmpty_Fails) {
    auto r = BaseReq();
    r.scope = MemoryScope::kUser;
    r.user_id = "";
    auto s = r.Validate();
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kInvalidArgument);
    EXPECT_NE(s.message().find("user_id"), std::string::npos);
}

// 2. user_id present, scope=kUser -> Ok
TEST_F(Mem05IsolationTest, Validate_UserIdPresent_Succeeds) {
    auto r = BaseReq();
    r.scope = MemoryScope::kUser;
    r.user_id = "u1";
    EXPECT_TRUE(r.Validate().ok());
}

// 3. scope=kSession, user_id empty -> InvalidArgument (user_id checked before session)
TEST_F(Mem05IsolationTest, Validate_SessionScope_UserIdRequired) {
    auto r = BaseReq();
    r.scope = MemoryScope::kSession;
    r.session_id = "s1";
    r.user_id = "";
    auto s = r.Validate();
    EXPECT_FALSE(s.ok());
    EXPECT_NE(s.message().find("user_id"), std::string::npos);
}

// 4. scope=kSession, user_id + session_id present -> Ok
TEST_F(Mem05IsolationTest, Validate_SessionScope_WithUserId_Succeeds) {
    auto r = BaseReq();
    r.scope = MemoryScope::kSession;
    r.user_id = "u1";
    r.session_id = "s1";
    EXPECT_TRUE(r.Validate().ok());
}

// 12. user_id too long -> rejected
TEST_F(Mem05IsolationTest, UserIdFormat_TooLong_Rejected) {
    auto r = BaseReq();
    r.user_id = std::string(200, 'a');  // 200 chars > 128
    auto s = r.Validate();
    EXPECT_FALSE(s.ok());
    EXPECT_NE(s.message().find("format"), std::string::npos);
}

// 13. user_id valid chars -> accepted
TEST_F(Mem05IsolationTest, UserIdFormat_ValidChars_Accepted) {
    auto r = BaseReq();
    r.user_id = "user-123_abc";
    EXPECT_TRUE(r.Validate().ok());
}

// Extra: exactly 128 chars (boundary) accepted; non-ASCII rejected.
TEST_F(Mem05IsolationTest, UserIdFormat_Boundary128_Accepted) {
    auto r = BaseReq();
    r.user_id = std::string(128, 'x');
    EXPECT_TRUE(r.Validate().ok());
}

TEST_F(Mem05IsolationTest, UserIdFormat_NonAscii_Rejected) {
    auto r = BaseReq();
    r.user_id = "user\x01ctrl";  // control char 0x01
    auto s = r.Validate();
    EXPECT_FALSE(s.ok());
    EXPECT_NE(s.message().find("format"), std::string::npos);
}

// ===== § 8.1 case 5-11: MatchScope() =====

// 5. metadata.user_id != request.user_id -> false
TEST_F(Mem05IsolationTest, MatchScope_UserIdMismatch_Excluded) {
    auto r = BaseReq();
    r.scope = MemoryScope::kUser;
    r.user_id = "u2";
    json meta;
    meta["user_id"] = "u1";
    EXPECT_FALSE(MatchScope(meta, r));
}

// 6. metadata.user_id == request.user_id, scope=kUser -> true
TEST_F(Mem05IsolationTest, MatchScope_UserIdMatch_Included) {
    auto r = BaseReq();
    r.scope = MemoryScope::kUser;
    r.user_id = "u1";
    json meta;
    meta["user_id"] = "u1";
    EXPECT_TRUE(MatchScope(meta, r));
}

// 7. metadata has no user_id -> false
TEST_F(Mem05IsolationTest, MatchScope_NoUserIdInMetadata_Excluded) {
    auto r = BaseReq();
    r.scope = MemoryScope::kUser;
    r.user_id = "u1";
    json meta;
    meta["session_id"] = "s1";  // no user_id
    EXPECT_FALSE(MatchScope(meta, r));
}

// 8. metadata.user_id == "" -> false (mismatch with non-empty request)
TEST_F(Mem05IsolationTest, MatchScope_EmptyUserIdInMetadata_Excluded) {
    auto r = BaseReq();
    r.scope = MemoryScope::kUser;
    r.user_id = "u1";
    json meta;
    meta["user_id"] = "";
    EXPECT_FALSE(MatchScope(meta, r));
}

// 9. scope=kSession, wrong user -> false
TEST_F(Mem05IsolationTest, MatchScope_SessionScope_WrongUser_Excluded) {
    auto r = BaseReq();
    r.scope = MemoryScope::kSession;
    r.user_id = "u1";
    r.session_id = "s1";
    json meta;
    meta["user_id"] = "u2";       // wrong user
    meta["session_id"] = "s1";
    EXPECT_FALSE(MatchScope(meta, r));
}

// 10. scope=kSession, right user + right session -> true
TEST_F(Mem05IsolationTest, MatchScope_SessionScope_RightUser_RightSession) {
    auto r = BaseReq();
    r.scope = MemoryScope::kSession;
    r.user_id = "u1";
    r.session_id = "s1";
    json meta;
    meta["user_id"] = "u1";
    meta["session_id"] = "s1";
    EXPECT_TRUE(MatchScope(meta, r));
}

// 11. scope=kSession, right user + wrong session -> false
TEST_F(Mem05IsolationTest, MatchScope_SessionScope_RightUser_WrongSession) {
    auto r = BaseReq();
    r.scope = MemoryScope::kSession;
    r.user_id = "u1";
    r.session_id = "s1";
    json meta;
    meta["user_id"] = "u1";
    meta["session_id"] = "s2";   // wrong session
    EXPECT_FALSE(MatchScope(meta, r));
}

// Extra: null metadata -> false (no user_id at all).
TEST_F(Mem05IsolationTest, MatchScope_NullMetadata_Excluded) {
    auto r = BaseReq();
    r.scope = MemoryScope::kUser;
    r.user_id = "u1";
    EXPECT_FALSE(MatchScope(json(), r));
}

// Extra: user_id is a non-string JSON type -> excluded.
TEST_F(Mem05IsolationTest, MatchScope_UserIdNotString_Excluded) {
    auto r = BaseReq();
    r.scope = MemoryScope::kUser;
    r.user_id = "u1";
    json meta;
    meta["user_id"] = 42;  // number, not string
    EXPECT_FALSE(MatchScope(meta, r));
}

}  // namespace
}  // namespace cortrix
