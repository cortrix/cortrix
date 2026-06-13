#include <gtest/gtest.h>
#include "cortrix/memory/memory_store.h"
#include "cortrix/memory/memory_injector.h"
#include "cortrix/memory/memory_config.h"
#include <chrono>
#include <thread>

namespace cortrix {
namespace {

// Minimal CortrixStore stub for integration tests
class StubCortrixStore : public CortrixStore {
public:
    int Open() override { return 0; }
    int Close() override { return 0; }
    // [D-I6] doc_id is a ULID string post id-system migration; the CortrixStore virtuals
    // now take const std::string&. (MemoryStore only needs this stub to construct — the
    // memory tests drive the :memory: SQLite MemoryStore, not these doc/block methods.)
    int doc_create(CortrixDoc& doc) override {
        doc.doc_id = "stub-doc-" + std::to_string(++next_doc_id_);
        return 0;
    }
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
    int64_t next_doc_id_ = 100;
};

class MemoryAPITest : public ::testing::Test {
protected:
    void SetUp() override {
        mem_store_ = std::make_unique<MemoryStore>(stub_store_);
        auto s = mem_store_->Init(":memory:");
        ASSERT_TRUE(s.ok()) << s.message();

        config_.inject_recent_turns = 3;
        config_.inject_max_tokens = 2000;
        config_.text_to_sql_ttl_seconds = 86400;
        config_.default_ttl_seconds = 0;
    }

    // Helper: create session and return session_id
    std::string CreateSession(const std::string& ns = "default",
                              const std::string& user_id = "") {
        MemorySession session;
        session.namespace_name = ns;
        session.user_id = user_id;
        auto s = mem_store_->SessionCreate(session);
        EXPECT_TRUE(s.ok()) << s.message();
        return session.session_id;
    }

    // Helper: write N turns of Q+A interaction pairs
    void WriteTurns(const std::string& session_id,
                    const std::string& ns,
                    int num_turns,
                    const std::string& user_id = "",
                    const std::string& query_prefix = "Question",
                    const std::string& answer_prefix = "Answer") {
        for (int i = 0; i < num_turns; i++) {
            InteractionLog user_log;
            user_log.session_id = session_id;
            user_log.namespace_name = ns;
            user_log.user_id = user_id;
            user_log.role = "user";
            user_log.content = query_prefix + " " + std::to_string(i);
            user_log.query_type = "chat";
            mem_store_->InteractionInsert(user_log);

            InteractionLog asst_log;
            asst_log.session_id = session_id;
            asst_log.namespace_name = ns;
            asst_log.user_id = user_id;
            asst_log.role = "assistant";
            asst_log.content = answer_prefix + " " + std::to_string(i);
            asst_log.status = "success";
            mem_store_->InteractionInsert(asst_log);

            mem_store_->SessionTouch(session_id);
        }
    }

    StubCortrixStore stub_store_;
    std::unique_ptr<MemoryStore> mem_store_;
    MemoryConfig config_;
};

// Test Case 1: End-to-end memory roundtrip
// Create session -> write interactions -> search -> delete -> verify gone
TEST_F(MemoryAPITest, EndToEndMemoryRoundtrip) {
    // 1. Create session
    std::string sid = CreateSession("default", "user_001");
    ASSERT_FALSE(sid.empty());

    // 2. Write 3 turns of Q+A
    WriteTurns(sid, "default", 3, "user_001", "What is Q4 revenue", "Revenue was $12.5M turn");

    // 3. Verify interactions stored: 3 turns * 2 interactions = 6
    int64_t count = 0;
    mem_store_->InteractionCount(sid, &count);
    EXPECT_EQ(count, 6);

    // 4. Search for "revenue" - should find interactions
    std::vector<InteractionLog> search_results;
    auto s = mem_store_->InteractionSearch("default", "revenue", "", "", 10, search_results);
    ASSERT_TRUE(s.ok()) << s.message();
    EXPECT_GT(search_results.size(), 0u);

    // Verify search results contain our content
    bool found_query = false;
    bool found_answer = false;
    for (const auto& r : search_results) {
        if (r.content.find("Q4 revenue") != std::string::npos) found_query = true;
        if (r.content.find("Revenue was") != std::string::npos) found_answer = true;
    }
    EXPECT_TRUE(found_query || found_answer) << "Search should find revenue-related interactions";

    // 5. Get session detail - verify all 6 interactions
    std::vector<InteractionLog> interactions;
    s = mem_store_->InteractionListBySession(sid, interactions);
    ASSERT_TRUE(s.ok());
    EXPECT_EQ(interactions.size(), 6u);

    // 6. Delete session (cascade delete)
    s = mem_store_->SessionDelete(sid);
    ASSERT_TRUE(s.ok());

    // 7. Verify session is gone (NotFound)
    MemorySession check;
    s = mem_store_->SessionGet(sid, check);
    EXPECT_EQ(s.code(), StatusCode::kNotFound);

    // 8. Verify interactions are gone
    mem_store_->InteractionCount(sid, &count);
    EXPECT_EQ(count, 0);

    // 9. Search after delete - should not find deleted content
    search_results.clear();
    s = mem_store_->InteractionSearch("default", "revenue", sid, "", 10, search_results);
    ASSERT_TRUE(s.ok());
    EXPECT_EQ(search_results.size(), 0u);
}

// Test Case 2: Scope filtering - search within specific session
TEST_F(MemoryAPITest, ScopeFilteringBySession) {
    // 1. Create 2 sessions
    std::string sid1 = CreateSession("default", "user_A");
    std::string sid2 = CreateSession("default", "user_B");

    // 2. Write interactions to both sessions with different content
    WriteTurns(sid1, "default", 2, "user_A", "Session1 project alpha", "Alpha response");
    WriteTurns(sid2, "default", 2, "user_B", "Session2 project beta", "Beta response");

    // 3. Search with scope=session1 -> only session1 results
    std::vector<InteractionLog> results;
    auto s = mem_store_->InteractionSearch("default", "project", sid1, "", 20, results);
    ASSERT_TRUE(s.ok());
    // Should only contain session1 interactions
    for (const auto& r : results) {
        EXPECT_EQ(r.session_id, sid1)
            << "Session-scoped search should only return results from session1";
    }

    // 4. Search with scope=session2
    results.clear();
    s = mem_store_->InteractionSearch("default", "project", sid2, "", 20, results);
    ASSERT_TRUE(s.ok());
    for (const auto& r : results) {
        EXPECT_EQ(r.session_id, sid2);
    }

    // 5. Search all sessions (no session filter)
    results.clear();
    s = mem_store_->InteractionSearch("default", "project", "", "", 20, results);
    ASSERT_TRUE(s.ok());
    EXPECT_GE(results.size(), 4u);  // At least 4 (2 turns * 2 sessions, user role only matched)
}

// Test Case 3: Scope filtering by user
TEST_F(MemoryAPITest, ScopeFilteringByUser) {
    std::string sid = CreateSession("default");

    // Insert interactions from different users
    InteractionLog log1;
    log1.session_id = sid;
    log1.namespace_name = "default";
    log1.user_id = "alice";
    log1.role = "user";
    log1.content = "user-specific content delta";
    mem_store_->InteractionInsert(log1);

    InteractionLog log2;
    log2.session_id = sid;
    log2.namespace_name = "default";
    log2.user_id = "bob";
    log2.role = "user";
    log2.content = "user-specific content delta";
    mem_store_->InteractionInsert(log2);

    // Search filtered by user=alice
    std::vector<InteractionLog> results;
    auto s = mem_store_->InteractionSearch("default", "delta", "", "alice", 10, results);
    ASSERT_TRUE(s.ok());
    EXPECT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].user_id, "alice");
}

// Test Case 4: Agent memory injection - recent context retrieval
TEST_F(MemoryAPITest, AgentMemoryInjection) {
    std::string sid = CreateSession("default");

    // 1. Write 10 interaction turns
    WriteTurns(sid, "default", 10);

    // 2. inject_recent_turns=3 -> return last 3 turns
    MemoryInjector injector(*mem_store_, config_);
    auto ctx = injector.GetRecentContext(sid);

    EXPECT_EQ(ctx.turn_count, 3);
    EXPECT_GT(ctx.token_count_approx, 0);
    EXPECT_FALSE(ctx.context_text.empty());

    // Should contain turns 7, 8, 9 (the last 3)
    EXPECT_NE(ctx.context_text.find("Question 7"), std::string::npos);
    EXPECT_NE(ctx.context_text.find("Answer 9"), std::string::npos);

    // Should NOT contain turns 0-6
    EXPECT_EQ(ctx.context_text.find("Question 0"), std::string::npos);
    EXPECT_EQ(ctx.context_text.find("Question 6"), std::string::npos);
}

// Test Case 5: Namespace isolation
TEST_F(MemoryAPITest, NamespaceIsolation) {
    std::string sid_a = CreateSession("ns_alpha");
    std::string sid_b = CreateSession("ns_beta");

    WriteTurns(sid_a, "ns_alpha", 2, "", "Isolated content epsilon", "Epsilon answer");
    WriteTurns(sid_b, "ns_beta", 2, "", "Isolated content epsilon", "Epsilon answer");

    // Search in ns_alpha only
    std::vector<InteractionLog> results;
    auto s = mem_store_->InteractionSearch("ns_alpha", "epsilon", "", "", 20, results);
    ASSERT_TRUE(s.ok());
    for (const auto& r : results) {
        EXPECT_EQ(r.namespace_name, "ns_alpha")
            << "Namespace search should only return results from ns_alpha";
    }

    // Search in ns_beta only
    results.clear();
    s = mem_store_->InteractionSearch("ns_beta", "epsilon", "", "", 20, results);
    ASSERT_TRUE(s.ok());
    for (const auto& r : results) {
        EXPECT_EQ(r.namespace_name, "ns_beta");
    }
}

// Test Case 6: Session metadata updates (touch increments count)
TEST_F(MemoryAPITest, SessionMetadataUpdates) {
    std::string sid = CreateSession("default");

    // Verify initial state
    MemorySession session;
    auto s = mem_store_->SessionGet(sid, session);
    ASSERT_TRUE(s.ok());
    EXPECT_EQ(session.interaction_count, 0);
    std::string original_created = session.created_at;

    // Small delay to ensure updated_at will differ from created_at
    std::this_thread::sleep_for(std::chrono::milliseconds(15));

    // Write 3 turns (each calls SessionTouch)
    WriteTurns(sid, "default", 3);

    // Verify interaction_count updated
    s = mem_store_->SessionGet(sid, session);
    ASSERT_TRUE(s.ok());
    EXPECT_EQ(session.interaction_count, 3);  // 3 touches for 3 turns

    // Verify updated_at changed (after the sleep, timestamps should differ)
    EXPECT_NE(session.updated_at, original_created);
}

// Test Case 7: Session list pagination (total_count + has_more per API spec)
TEST_F(MemoryAPITest, SessionListPaginationTotalCount) {
    // Design spec (API_GATEWAY_DESIGN.md 2.4.4):
    //   Response must include total_count and has_more
    //   Default limit = 20

    // Create 5 sessions
    for (int i = 0; i < 5; i++) {
        CreateSession("default", "user_" + std::to_string(i));
    }

    // Verify SessionCount
    int64_t total = 0;
    auto s = mem_store_->SessionCount("default", &total);
    ASSERT_TRUE(s.ok());
    EXPECT_EQ(total, 5);

    // Get page 1 (limit=2, offset=0)
    std::vector<MemorySession> sessions;
    s = mem_store_->SessionList("default", 2, 0, sessions);
    ASSERT_TRUE(s.ok());
    EXPECT_EQ(sessions.size(), 2u);
    // has_more: (0 + 2) < 5 => true
    bool has_more = (0 + static_cast<int>(sessions.size())) < static_cast<int>(total);
    EXPECT_TRUE(has_more);

    // Get page 3 (limit=2, offset=4)
    sessions.clear();
    s = mem_store_->SessionList("default", 2, 4, sessions);
    ASSERT_TRUE(s.ok());
    EXPECT_EQ(sessions.size(), 1u);
    // has_more: (4 + 1) < 5 => false
    has_more = (4 + static_cast<int>(sessions.size())) < static_cast<int>(total);
    EXPECT_FALSE(has_more);
}

// Test Case 8: SessionCount isolation by namespace
TEST_F(MemoryAPITest, SessionCountNamespaceIsolation) {
    CreateSession("ns_a");
    CreateSession("ns_a");
    CreateSession("ns_b");

    int64_t count_a = 0, count_b = 0;
    mem_store_->SessionCount("ns_a", &count_a);
    mem_store_->SessionCount("ns_b", &count_b);

    EXPECT_EQ(count_a, 2);
    EXPECT_EQ(count_b, 1);
}

}  // namespace
}  // namespace cortrix
