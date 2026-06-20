#include <gtest/gtest.h>
#include "cortrix/memory/memory_store.h"
#include <thread>
#include <regex>
#include <set>

namespace cortrix {
namespace {

// Minimal CortrixStore stub — MemoryStore only needs CortrixStore& for doc operations;
// interaction_log/memory_sessions use their own SQLite connection.
class NullCortrixStore : public CortrixStore {
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
    int doc_count(int64_t* c) override { *c = 0; return 0; }
    int block_insert(CortrixBlock&) override { return 0; }
    int block_get(uint64_t, CortrixBlock&) override { return -2; }
    int block_get_by_doc(const std::string&, std::vector<CortrixBlock>&) override { return 0; }
    int block_delete_by_doc(const std::string&) override { return 0; }
    int block_count(int64_t* c) override { *c = 0; return 0; }
    int search_fulltext(const std::string&, int, std::vector<SearchResult>&) override { return 0; }
    int search_metadata(const std::string&, int, std::vector<SearchResult>&) override { return -1; }
};

class MemoryStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        store_ = std::make_unique<MemoryStore>(null_store_);
        auto s = store_->Init(":memory:");
        ASSERT_TRUE(s.ok()) << s.message();
    }

    NullCortrixStore null_store_;
    std::unique_ptr<MemoryStore> store_;
};

// --- DDL Tests ---

TEST_F(MemoryStoreTest, InitIdempotent) {
    // Second store on a different in-memory DB should work
    MemoryStore store2(null_store_);
    auto s = store2.Init(":memory:");
    EXPECT_TRUE(s.ok()) << s.message();
}

// --- UUID Tests ---

TEST_F(MemoryStoreTest, GenerateUUIDFormat) {
    MemorySession session;
    session.namespace_name = "test";
    auto s = store_->SessionCreate(session);
    ASSERT_TRUE(s.ok()) << s.message();

    // UUID v4 format: 8-4-4-4-12 hex, version nibble=4, variant bits=8/9/a/b
    std::regex uuid_regex(
        "^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$");
    EXPECT_TRUE(std::regex_match(session.session_id, uuid_regex))
        << "UUID format invalid: " << session.session_id;
}

TEST_F(MemoryStoreTest, GenerateUUIDUnique) {
    std::set<std::string> ids;
    for (int i = 0; i < 100; i++) {
        MemorySession session;
        session.namespace_name = "test";
        auto s = store_->SessionCreate(session);
        ASSERT_TRUE(s.ok());
        EXPECT_TRUE(ids.insert(session.session_id).second)
            << "Duplicate UUID: " << session.session_id;
    }
}

// --- Session CRUD ---

TEST_F(MemoryStoreTest, SessionCreateAndGet) {
    MemorySession session;
    session.namespace_name = "default";
    session.user_id = "user_001";
    session.title = "Test Session";

    auto s = store_->SessionCreate(session);
    ASSERT_TRUE(s.ok()) << s.message();
    EXPECT_FALSE(session.session_id.empty());
    EXPECT_FALSE(session.created_at.empty());
    EXPECT_EQ(session.interaction_count, 0);

    MemorySession got;
    s = store_->SessionGet(session.session_id, got);
    ASSERT_TRUE(s.ok()) << s.message();
    EXPECT_EQ(got.session_id, session.session_id);
    EXPECT_EQ(got.namespace_name, "default");
    EXPECT_EQ(got.user_id, "user_001");
    EXPECT_EQ(got.title, "Test Session");
    EXPECT_EQ(got.interaction_count, 0);
}

TEST_F(MemoryStoreTest, SessionNotFound) {
    MemorySession session;
    auto s = store_->SessionGet("nonexistent-id", session);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kNotFound);
}

TEST_F(MemoryStoreTest, SessionListPagination) {
    for (int i = 0; i < 5; i++) {
        MemorySession session;
        session.namespace_name = "default";
        session.title = "Session " + std::to_string(i);
        store_->SessionCreate(session);
    }

    std::vector<MemorySession> sessions;
    auto s = store_->SessionList("default", 2, 0, sessions);
    ASSERT_TRUE(s.ok());
    EXPECT_EQ(sessions.size(), 2u);

    sessions.clear();
    s = store_->SessionList("default", 2, 3, sessions);
    ASSERT_TRUE(s.ok());
    EXPECT_EQ(sessions.size(), 2u);

    sessions.clear();
    s = store_->SessionList("default", 10, 0, sessions);
    ASSERT_TRUE(s.ok());
    EXPECT_EQ(sessions.size(), 5u);
}

TEST_F(MemoryStoreTest, SessionListFilterByNamespace) {
    MemorySession s1; s1.namespace_name = "ns_a"; store_->SessionCreate(s1);
    MemorySession s2; s2.namespace_name = "ns_b"; store_->SessionCreate(s2);
    MemorySession s3; s3.namespace_name = "ns_a"; store_->SessionCreate(s3);

    std::vector<MemorySession> sessions;
    store_->SessionList("ns_a", 50, 0, sessions);
    EXPECT_EQ(sessions.size(), 2u);
}

// R10 GAP-E1 regression: the per-user filter must be applied IN SQL so that
// SessionList rows and SessionCount agree under MEM05 isolation. The pre-fix bug
// post-filtered an NS-wide page in the app layer while total_count stayed NS-wide,
// yielding total_count > returned rows (has_more=true with 0 items → infinite paging).
TEST_F(MemoryStoreTest, SessionListAndCountScopedByUserId) {
    MemorySession a1; a1.namespace_name = "default"; a1.user_id = "userA"; store_->SessionCreate(a1);
    MemorySession a2; a2.namespace_name = "default"; a2.user_id = "userA"; store_->SessionCreate(a2);
    MemorySession b1; b1.namespace_name = "default"; b1.user_id = "userB"; store_->SessionCreate(b1);

    // List scoped to userA returns only userA's 2 sessions.
    std::vector<MemorySession> sessions;
    auto s = store_->SessionList("default", 20, 0, sessions, "userA");
    ASSERT_TRUE(s.ok());
    EXPECT_EQ(sessions.size(), 2u);
    for (const auto& sess : sessions) EXPECT_EQ(sess.user_id, "userA");

    // Count scoped to userA == filtered list size (the has_more invariant).
    int64_t count = 0;
    ASSERT_TRUE(store_->SessionCount("default", &count, "userA").ok());
    EXPECT_EQ(count, 2);

    // A user with no sessions: empty list AND zero count (no phantom has_more).
    sessions.clear();
    ASSERT_TRUE(store_->SessionList("default", 20, 0, sessions, "ghost").ok());
    EXPECT_EQ(sessions.size(), 0u);
    int64_t ghost_count = -1;
    ASSERT_TRUE(store_->SessionCount("default", &ghost_count, "ghost").ok());
    EXPECT_EQ(ghost_count, 0);

    // Empty user_id = NS-wide (unchanged legacy behavior): all 3 sessions.
    int64_t all = 0;
    ASSERT_TRUE(store_->SessionCount("default", &all).ok());
    EXPECT_EQ(all, 3);
}

TEST_F(MemoryStoreTest, SessionDeleteCascade) {
    MemorySession session;
    session.namespace_name = "default";
    store_->SessionCreate(session);

    for (int i = 0; i < 4; i++) {
        InteractionLog log;
        log.session_id = session.session_id;
        log.namespace_name = "default";
        log.role = (i % 2 == 0) ? "user" : "assistant";
        log.content = "Message " + std::to_string(i);
        store_->InteractionInsert(log);
    }

    int64_t count = 0;
    store_->InteractionCount(session.session_id, &count);
    EXPECT_EQ(count, 4);

    auto s = store_->SessionDelete(session.session_id);
    ASSERT_TRUE(s.ok());

    MemorySession check;
    s = store_->SessionGet(session.session_id, check);
    EXPECT_EQ(s.code(), StatusCode::kNotFound);

    store_->InteractionCount(session.session_id, &count);
    EXPECT_EQ(count, 0);
}

TEST_F(MemoryStoreTest, SessionDeleteNotFound) {
    auto s = store_->SessionDelete("nonexistent-id");
    EXPECT_EQ(s.code(), StatusCode::kNotFound);
}

// --- Interaction CRUD ---

TEST_F(MemoryStoreTest, InteractionInsertAndList) {
    MemorySession session;
    session.namespace_name = "default";
    store_->SessionCreate(session);

    for (int i = 0; i < 3; i++) {
        InteractionLog log;
        log.session_id = session.session_id;
        log.namespace_name = "default";
        log.user_id = "user_001";
        log.role = (i % 2 == 0) ? "user" : "assistant";
        log.content = "Message " + std::to_string(i);
        log.query_type = "chat";
        auto s = store_->InteractionInsert(log);
        ASSERT_TRUE(s.ok()) << s.message();
        EXPECT_FALSE(log.id.empty());
        EXPECT_FALSE(log.created_at.empty());
    }

    std::vector<InteractionLog> interactions;
    auto s = store_->InteractionListBySession(session.session_id, interactions);
    ASSERT_TRUE(s.ok());
    EXPECT_EQ(interactions.size(), 3u);
    EXPECT_EQ(interactions[0].content, "Message 0");
    EXPECT_EQ(interactions[2].content, "Message 2");
}

TEST_F(MemoryStoreTest, InteractionGetRecent) {
    MemorySession session;
    session.namespace_name = "default";
    store_->SessionCreate(session);

    for (int i = 0; i < 10; i++) {
        InteractionLog log;
        log.session_id = session.session_id;
        log.namespace_name = "default";
        log.role = "user";
        log.content = "Message " + std::to_string(i);
        store_->InteractionInsert(log);
    }

    std::vector<InteractionLog> interactions;
    auto s = store_->InteractionGetRecent(session.session_id, 3, interactions);
    ASSERT_TRUE(s.ok());
    EXPECT_EQ(interactions.size(), 3u);
    // DESC order: most recent first
    EXPECT_EQ(interactions[0].content, "Message 9");
    EXPECT_EQ(interactions[1].content, "Message 8");
    EXPECT_EQ(interactions[2].content, "Message 7");
}

TEST_F(MemoryStoreTest, InteractionCount) {
    MemorySession session;
    session.namespace_name = "default";
    store_->SessionCreate(session);

    int64_t count = 0;
    store_->InteractionCount(session.session_id, &count);
    EXPECT_EQ(count, 0);

    for (int i = 0; i < 5; i++) {
        InteractionLog log;
        log.session_id = session.session_id;
        log.namespace_name = "default";
        log.role = "user";
        log.content = "x";
        store_->InteractionInsert(log);
    }

    store_->InteractionCount(session.session_id, &count);
    EXPECT_EQ(count, 5);
}

TEST_F(MemoryStoreTest, SessionTouch) {
    MemorySession session;
    session.namespace_name = "default";
    store_->SessionCreate(session);

    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    auto s = store_->SessionTouch(session.session_id);
    ASSERT_TRUE(s.ok());

    MemorySession got;
    store_->SessionGet(session.session_id, got);
    EXPECT_EQ(got.interaction_count, 1);

    store_->SessionTouch(session.session_id);
    store_->SessionGet(session.session_id, got);
    EXPECT_EQ(got.interaction_count, 2);
}

TEST_F(MemoryStoreTest, SessionTouchNotFound) {
    auto s = store_->SessionTouch("nonexistent");
    EXPECT_EQ(s.code(), StatusCode::kNotFound);
}

// --- Concurrent Test ---

TEST_F(MemoryStoreTest, ConcurrentInsert) {
    MemorySession session;
    session.namespace_name = "default";
    store_->SessionCreate(session);

    const int per_thread = 20;
    auto inserter = [&](int thread_id) {
        for (int i = 0; i < per_thread; i++) {
            InteractionLog log;
            log.session_id = session.session_id;
            log.namespace_name = "default";
            log.role = "user";
            log.content = "T" + std::to_string(thread_id) + "_" + std::to_string(i);
            store_->InteractionInsert(log);
        }
    };

    std::thread t1(inserter, 1);
    std::thread t2(inserter, 2);
    t1.join();
    t2.join();

    int64_t count = 0;
    store_->InteractionCount(session.session_id, &count);
    EXPECT_EQ(count, per_thread * 2);
}

// --- Search Tests ---

TEST_F(MemoryStoreTest, InteractionSearchBasic) {
    MemorySession session;
    session.namespace_name = "default";
    store_->SessionCreate(session);

    // Insert some interactions with different content
    std::vector<std::string> contents = {
        "What is Q4 revenue?",
        "Q4 revenue was $12.5M.",
        "Tell me about expenses",
        "Expenses totaled $8.2M",
        "How about profit margin?"
    };
    for (const auto& c : contents) {
        InteractionLog log;
        log.session_id = session.session_id;
        log.namespace_name = "default";
        log.role = "user";
        log.content = c;
        store_->InteractionInsert(log);
    }

    // Search for "revenue" - should find 2 results
    std::vector<InteractionLog> results;
    auto s = store_->InteractionSearch("default", "revenue", "", "", 10, results);
    ASSERT_TRUE(s.ok()) << s.message();
    EXPECT_EQ(results.size(), 2u);

    // Search for "expenses" - should find 1 result (case-insensitive check)
    results.clear();
    s = store_->InteractionSearch("default", "expenses", "", "", 10, results);
    ASSERT_TRUE(s.ok());
    EXPECT_GE(results.size(), 1u);

    // Search for something that doesn't exist
    results.clear();
    s = store_->InteractionSearch("default", "nonexistent_xyz", "", "", 10, results);
    ASSERT_TRUE(s.ok());
    EXPECT_EQ(results.size(), 0u);
}

TEST_F(MemoryStoreTest, InteractionSearchBySession) {
    MemorySession s1; s1.namespace_name = "default"; store_->SessionCreate(s1);
    MemorySession s2; s2.namespace_name = "default"; store_->SessionCreate(s2);

    InteractionLog log1;
    log1.session_id = s1.session_id;
    log1.namespace_name = "default";
    log1.role = "user";
    log1.content = "alpha content here";
    store_->InteractionInsert(log1);

    InteractionLog log2;
    log2.session_id = s2.session_id;
    log2.namespace_name = "default";
    log2.role = "user";
    log2.content = "alpha content there";
    store_->InteractionInsert(log2);

    // Search with session filter: only session 1
    std::vector<InteractionLog> results;
    auto s = store_->InteractionSearch("default", "alpha", s1.session_id, "", 10, results);
    ASSERT_TRUE(s.ok());
    EXPECT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].session_id, s1.session_id);
}

TEST_F(MemoryStoreTest, InteractionSearchByUser) {
    MemorySession session;
    session.namespace_name = "default";
    store_->SessionCreate(session);

    InteractionLog log1;
    log1.session_id = session.session_id;
    log1.namespace_name = "default";
    log1.role = "user";
    log1.user_id = "user_A";
    log1.content = "beta search term";
    store_->InteractionInsert(log1);

    InteractionLog log2;
    log2.session_id = session.session_id;
    log2.namespace_name = "default";
    log2.role = "user";
    log2.user_id = "user_B";
    log2.content = "beta search term also";
    store_->InteractionInsert(log2);

    // Search with user filter
    std::vector<InteractionLog> results;
    auto s = store_->InteractionSearch("default", "beta", "", "user_A", 10, results);
    ASSERT_TRUE(s.ok());
    EXPECT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].user_id, "user_A");
}

TEST_F(MemoryStoreTest, InteractionSearchNamespaceIsolation) {
    MemorySession sa; sa.namespace_name = "ns_x"; store_->SessionCreate(sa);
    MemorySession sb; sb.namespace_name = "ns_y"; store_->SessionCreate(sb);

    InteractionLog log1;
    log1.session_id = sa.session_id;
    log1.namespace_name = "ns_x";
    log1.role = "user";
    log1.content = "gamma test data";
    store_->InteractionInsert(log1);

    InteractionLog log2;
    log2.session_id = sb.session_id;
    log2.namespace_name = "ns_y";
    log2.role = "user";
    log2.content = "gamma test data";
    store_->InteractionInsert(log2);

    // Search in ns_x only
    std::vector<InteractionLog> results;
    auto s = store_->InteractionSearch("ns_x", "gamma", "", "", 10, results);
    ASSERT_TRUE(s.ok());
    EXPECT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].namespace_name, "ns_x");
}

TEST_F(MemoryStoreTest, InteractionSearchLimit) {
    MemorySession session;
    session.namespace_name = "default";
    store_->SessionCreate(session);

    for (int i = 0; i < 20; i++) {
        InteractionLog log;
        log.session_id = session.session_id;
        log.namespace_name = "default";
        log.role = "user";
        log.content = "delta item " + std::to_string(i);
        store_->InteractionInsert(log);
    }

    std::vector<InteractionLog> results;
    auto s = store_->InteractionSearch("default", "delta", "", "", 5, results);
    ASSERT_TRUE(s.ok());
    EXPECT_EQ(results.size(), 5u);
}

TEST_F(MemoryStoreTest, InteractionSearchEmptyQuery) {
    std::vector<InteractionLog> results;
    auto s = store_->InteractionSearch("default", "", "", "", 10, results);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kInvalidArgument);
}

// --- LIKE wildcard escaping tests (M-FUNC-001) ---

TEST_F(MemoryStoreTest, InteractionSearchEscapesPercent) {
    MemorySession session;
    session.namespace_name = "default";
    store_->SessionCreate(session);

    // Insert content with literal % character
    InteractionLog log1;
    log1.session_id = session.session_id;
    log1.namespace_name = "default";
    log1.role = "user";
    log1.content = "The profit margin is 100% guaranteed";
    store_->InteractionInsert(log1);

    // Insert content WITHOUT %
    InteractionLog log2;
    log2.session_id = session.session_id;
    log2.namespace_name = "default";
    log2.role = "user";
    log2.content = "The profit margin is good";
    store_->InteractionInsert(log2);

    // Search for literal "100%" - should only match the first one
    std::vector<InteractionLog> results;
    auto s = store_->InteractionSearch("default", "100%", "", "", 10, results);
    ASSERT_TRUE(s.ok()) << s.message();
    EXPECT_EQ(results.size(), 1u);
    EXPECT_NE(results[0].content.find("100%"), std::string::npos);
}

TEST_F(MemoryStoreTest, InteractionSearchEscapesUnderscore) {
    MemorySession session;
    session.namespace_name = "default";
    store_->SessionCreate(session);

    // Insert content with literal _
    InteractionLog log1;
    log1.session_id = session.session_id;
    log1.namespace_name = "default";
    log1.role = "user";
    log1.content = "file_name_v2 was updated";
    store_->InteractionInsert(log1);

    // Insert content that would match single-char wildcard _ but not literal _
    InteractionLog log2;
    log2.session_id = session.session_id;
    log2.namespace_name = "default";
    log2.role = "user";
    log2.content = "filename v2 was updated";
    store_->InteractionInsert(log2);

    // Search for "file_name" - should only match the literal underscore
    std::vector<InteractionLog> results;
    auto s = store_->InteractionSearch("default", "file_name", "", "", 10, results);
    ASSERT_TRUE(s.ok()) << s.message();
    EXPECT_EQ(results.size(), 1u);
    EXPECT_NE(results[0].content.find("file_name"), std::string::npos);
}

TEST_F(MemoryStoreTest, InteractionSearchEscapesBackslash) {
    MemorySession session;
    session.namespace_name = "default";
    store_->SessionCreate(session);

    // Insert content with literal backslash
    InteractionLog log1;
    log1.session_id = session.session_id;
    log1.namespace_name = "default";
    log1.role = "user";
    log1.content = "path is C:\\Users\\test";
    store_->InteractionInsert(log1);

    InteractionLog log2;
    log2.session_id = session.session_id;
    log2.namespace_name = "default";
    log2.role = "user";
    log2.content = "path is C:/Users/test";
    store_->InteractionInsert(log2);

    // Search for literal backslash path
    std::vector<InteractionLog> results;
    auto s = store_->InteractionSearch("default", "C:\\Users", "", "", 10, results);
    ASSERT_TRUE(s.ok()) << s.message();
    EXPECT_EQ(results.size(), 1u);
    EXPECT_NE(results[0].content.find("C:\\Users"), std::string::npos);
}

TEST_F(MemoryStoreTest, InteractionSearchLimitClamp) {
    // Verify limit is clamped between 1 and 100
    MemorySession session;
    session.namespace_name = "default";
    store_->SessionCreate(session);

    InteractionLog log;
    log.session_id = session.session_id;
    log.namespace_name = "default";
    log.role = "user";
    log.content = "clamp test content";
    store_->InteractionInsert(log);

    // limit < 1 should be corrected to 10 (default)
    std::vector<InteractionLog> results;
    auto s = store_->InteractionSearch("default", "clamp", "", "", 0, results);
    ASSERT_TRUE(s.ok());
    EXPECT_EQ(results.size(), 1u);

    // limit > 100 should be clamped to 100
    results.clear();
    s = store_->InteractionSearch("default", "clamp", "", "", 200, results);
    ASSERT_TRUE(s.ok());
    EXPECT_EQ(results.size(), 1u);
}

// --- Optional fields ---

TEST_F(MemoryStoreTest, InteractionOptionalFields) {
    MemorySession session;
    session.namespace_name = "default";
    store_->SessionCreate(session);

    InteractionLog log;
    log.session_id = session.session_id;
    log.namespace_name = "default";
    log.role = "user";
    log.content = "Hello";
    store_->InteractionInsert(log);

    std::vector<InteractionLog> interactions;
    store_->InteractionListBySession(session.session_id, interactions);
    ASSERT_EQ(interactions.size(), 1u);
    EXPECT_EQ(interactions[0].user_id, "");
    EXPECT_EQ(interactions[0].query_type, "");
    EXPECT_EQ(interactions[0].status, "");
}

// --- SessionCount Tests (design spec: total_count for pagination) ---

TEST_F(MemoryStoreTest, SessionCountEmpty) {
    int64_t count = -1;
    auto s = store_->SessionCount("default", &count);
    ASSERT_TRUE(s.ok()) << s.message();
    EXPECT_EQ(count, 0);
}

TEST_F(MemoryStoreTest, SessionCountMultiple) {
    for (int i = 0; i < 7; i++) {
        MemorySession session;
        session.namespace_name = "default";
        store_->SessionCreate(session);
    }
    // Different namespace
    MemorySession other;
    other.namespace_name = "other_ns";
    store_->SessionCreate(other);

    int64_t count = 0;
    auto s = store_->SessionCount("default", &count);
    ASSERT_TRUE(s.ok());
    EXPECT_EQ(count, 7);

    s = store_->SessionCount("other_ns", &count);
    ASSERT_TRUE(s.ok());
    EXPECT_EQ(count, 1);

    s = store_->SessionCount("nonexistent", &count);
    ASSERT_TRUE(s.ok());
    EXPECT_EQ(count, 0);
}

TEST_F(MemoryStoreTest, SessionListDefaultLimitDesignSpec) {
    // Design spec (API_GATEWAY_DESIGN.md 2.4.4): default limit = 20
    // This test verifies that the SessionList function respects limit correctly
    for (int i = 0; i < 25; i++) {
        MemorySession session;
        session.namespace_name = "default";
        store_->SessionCreate(session);
    }

    // With limit=20 (design default), should return exactly 20
    std::vector<MemorySession> sessions;
    auto s = store_->SessionList("default", 20, 0, sessions);
    ASSERT_TRUE(s.ok());
    EXPECT_EQ(sessions.size(), 20u);

    // Verify has_more: offset=20, should get 5 remaining
    sessions.clear();
    s = store_->SessionList("default", 20, 20, sessions);
    ASSERT_TRUE(s.ok());
    EXPECT_EQ(sessions.size(), 5u);
}

// --- Error path: Init with bad path ---

TEST_F(MemoryStoreTest, InitBadPath) {
    MemoryStore store2(null_store_);
    // Use a path that cannot be opened (directory as file)
    auto s = store2.Init("/nonexistent/deeply/nested/path/db.sqlite");
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kInternal);
}

// --- SessionCreate with null/empty fields ---

TEST_F(MemoryStoreTest, SessionCreateEmptyNamespace) {
    MemorySession session;
    session.namespace_name = "";  // empty namespace
    auto s = store_->SessionCreate(session);
    // Should still succeed (no constraint on namespace_name being non-empty at store level)
    EXPECT_TRUE(s.ok());
}

TEST_F(MemoryStoreTest, SessionCreateWithDocId) {
    MemorySession session;
    session.namespace_name = "default";
    session.doc_id = "01JTESTDOC0000000000000042";

    auto s = store_->SessionCreate(session);
    ASSERT_TRUE(s.ok());

    MemorySession got;
    store_->SessionGet(session.session_id, got);
    EXPECT_EQ(got.doc_id, "01JTESTDOC0000000000000042");
}

// --- InteractionInsert with pre-set created_at ---

TEST_F(MemoryStoreTest, InteractionInsertPresetCreatedAt) {
    MemorySession session;
    session.namespace_name = "default";
    store_->SessionCreate(session);

    InteractionLog log;
    log.session_id = session.session_id;
    log.namespace_name = "default";
    log.role = "user";
    log.content = "Hello";
    log.created_at = "2026-01-01T00:00:00.000Z";

    auto s = store_->InteractionInsert(log);
    ASSERT_TRUE(s.ok());

    std::vector<InteractionLog> interactions;
    store_->InteractionListBySession(session.session_id, interactions);
    ASSERT_EQ(interactions.size(), 1u);
    EXPECT_EQ(interactions[0].created_at, "2026-01-01T00:00:00.000Z");
}

// --- InteractionInsert with all optional fields ---

TEST_F(MemoryStoreTest, InteractionInsertAllFields) {
    MemorySession session;
    session.namespace_name = "default";
    store_->SessionCreate(session);

    InteractionLog log;
    log.session_id = session.session_id;
    log.namespace_name = "default";
    log.user_id = "user_x";
    log.role = "assistant";
    log.content = "Response text";
    log.query_type = "semantic";
    log.status = "success";
    log.latency_ms = 250;
    log.metadata_json = R"({"key":"value"})";

    auto s = store_->InteractionInsert(log);
    ASSERT_TRUE(s.ok());

    std::vector<InteractionLog> interactions;
    store_->InteractionListBySession(session.session_id, interactions);
    ASSERT_EQ(interactions.size(), 1u);
    EXPECT_EQ(interactions[0].user_id, "user_x");
    EXPECT_EQ(interactions[0].query_type, "semantic");
    EXPECT_EQ(interactions[0].status, "success");
    EXPECT_EQ(interactions[0].latency_ms, 250);
    EXPECT_EQ(interactions[0].metadata_json, R"({"key":"value"})");
}

// --- SessionList empty namespace ---

TEST_F(MemoryStoreTest, SessionListEmptyNamespace) {
    std::vector<MemorySession> sessions;
    auto s = store_->SessionList("nonexistent_ns", 10, 0, sessions);
    ASSERT_TRUE(s.ok());
    EXPECT_EQ(sessions.size(), 0u);
}

// --- InteractionGetRecent with more than available ---

TEST_F(MemoryStoreTest, InteractionGetRecentMoreThanAvailable) {
    MemorySession session;
    session.namespace_name = "default";
    store_->SessionCreate(session);

    InteractionLog log;
    log.session_id = session.session_id;
    log.namespace_name = "default";
    log.role = "user";
    log.content = "only message";
    store_->InteractionInsert(log);

    std::vector<InteractionLog> interactions;
    auto s = store_->InteractionGetRecent(session.session_id, 100, interactions);
    ASSERT_TRUE(s.ok());
    EXPECT_EQ(interactions.size(), 1u);
}

// --- InteractionListBySession empty ---

TEST_F(MemoryStoreTest, InteractionListBySessionEmpty) {
    MemorySession session;
    session.namespace_name = "default";
    store_->SessionCreate(session);

    std::vector<InteractionLog> interactions;
    auto s = store_->InteractionListBySession(session.session_id, interactions);
    ASSERT_TRUE(s.ok());
    EXPECT_EQ(interactions.size(), 0u);
}

// --- InteractionCount for nonexistent session ---

TEST_F(MemoryStoreTest, InteractionCountNonexistentSession) {
    int64_t count = -1;
    auto s = store_->InteractionCount("nonexistent", &count);
    ASSERT_TRUE(s.ok());
    EXPECT_EQ(count, 0);
}

// --- NowISO8601 format validation ---

TEST_F(MemoryStoreTest, TimestampFormat) {
    MemorySession session;
    session.namespace_name = "default";
    store_->SessionCreate(session);

    // Verify ISO 8601 format: YYYY-MM-DDTHH:MM:SS.mmmZ
    std::regex iso_regex(
        R"(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z)");
    EXPECT_TRUE(std::regex_match(session.created_at, iso_regex))
        << "Timestamp format invalid: " << session.created_at;
}

// --- SessionGet / SessionList with NULL optional columns ---

// A session created with no user_id and no title leaves those columns NULL;
// SessionGet must coerce them to "" (the col2/col3 NULL ternary, lines 358/360).
TEST_F(MemoryStoreTest, SessionGetNullOptionalColumns) {
    MemorySession session;
    session.namespace_name = "default";
    // user_id + title intentionally left empty -> bound as NULL on create
    auto s = store_->SessionCreate(session);
    ASSERT_TRUE(s.ok());

    MemorySession got;
    s = store_->SessionGet(session.session_id, got);
    ASSERT_TRUE(s.ok());
    EXPECT_EQ(got.user_id, "");   // NULL column -> empty
    EXPECT_EQ(got.title, "");
    EXPECT_EQ(got.doc_id, "");
}

// SessionList must likewise coerce NULL user_id/title to "" (lines 400-403).
TEST_F(MemoryStoreTest, SessionListNullOptionalColumns) {
    MemorySession session;
    session.namespace_name = "ns_nulls";
    ASSERT_TRUE(store_->SessionCreate(session).ok());

    std::vector<MemorySession> sessions;
    auto s = store_->SessionList("ns_nulls", 10, 0, sessions);
    ASSERT_TRUE(s.ok());
    ASSERT_EQ(sessions.size(), 1u);
    EXPECT_EQ(sessions[0].user_id, "");
    EXPECT_EQ(sessions[0].title, "");
}

// --- InteractionPairInsertAndSessionTouch ---

TEST_F(MemoryStoreTest, InteractionPairInsertSuccessTouchesSession) {
    MemorySession session;
    session.namespace_name = "default";
    ASSERT_TRUE(store_->SessionCreate(session).ok());

    InteractionLog user_log;
    user_log.session_id = session.session_id;
    user_log.namespace_name = "default";
    user_log.role = "user";
    user_log.content = "question";
    InteractionLog assistant_log;
    assistant_log.session_id = session.session_id;
    assistant_log.namespace_name = "default";
    assistant_log.role = "assistant";
    assistant_log.content = "answer";

    auto s = store_->InteractionPairInsertAndSessionTouch(
        user_log, assistant_log, session.session_id);
    ASSERT_TRUE(s.ok()) << s.message();

    int64_t count = 0;
    store_->InteractionCount(session.session_id, &count);
    EXPECT_EQ(count, 2);
    MemorySession got;
    store_->SessionGet(session.session_id, got);
    EXPECT_EQ(got.interaction_count, 1);  // one TouchSessionLocked +1
}

// The pair-insert transaction rolls back when TouchSessionLocked fails on a
// nonexistent session (lines 755-758): the two interactions must not persist.
TEST_F(MemoryStoreTest, InteractionPairInsertRollsBackWhenSessionMissing) {
    const std::string ghost = "ghost-session-id";
    InteractionLog user_log;
    user_log.session_id = ghost;
    user_log.namespace_name = "default";
    user_log.role = "user";
    user_log.content = "q";
    InteractionLog assistant_log;
    assistant_log.session_id = ghost;
    assistant_log.namespace_name = "default";
    assistant_log.role = "assistant";
    assistant_log.content = "a";

    auto s = store_->InteractionPairInsertAndSessionTouch(
        user_log, assistant_log, ghost);
    ASSERT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kNotFound);

    // Transaction rolled back: no interactions remain for the ghost session.
    int64_t count = -1;
    store_->InteractionCount(ghost, &count);
    EXPECT_EQ(count, 0);
}

// --- Injected store-failure seam (F23 §4.5, SetFailNextOps) ---
// Each public op consumes one armed fault and returns Status::Internal before any
// SQL — this exercises the per-op fault-check branch the gate counts as missed.

TEST_F(MemoryStoreTest, SeamFailsSessionCreate) {
    MemorySession session;
    session.namespace_name = "default";
    store_->SetFailNextOps(1);
    auto s = store_->SessionCreate(session);
    EXPECT_EQ(s.code(), StatusCode::kInternal);
    EXPECT_NE(s.message().find("injected store failure"), std::string::npos);
}

TEST_F(MemoryStoreTest, SeamFailsSessionGet) {
    store_->SetFailNextOps(1);
    MemorySession got;
    EXPECT_EQ(store_->SessionGet("any-id", got).code(), StatusCode::kInternal);
}

TEST_F(MemoryStoreTest, SeamFailsSessionList) {
    store_->SetFailNextOps(1);
    std::vector<MemorySession> sessions;
    EXPECT_EQ(store_->SessionList("default", 10, 0, sessions).code(), StatusCode::kInternal);
}

TEST_F(MemoryStoreTest, SeamFailsSessionDelete) {
    store_->SetFailNextOps(1);
    EXPECT_EQ(store_->SessionDelete("any-id").code(), StatusCode::kInternal);
}

TEST_F(MemoryStoreTest, SeamFailsInteractionInsert) {
    store_->SetFailNextOps(1);
    InteractionLog log;
    log.session_id = "s";
    log.namespace_name = "default";
    log.role = "user";
    log.content = "c";
    EXPECT_EQ(store_->InteractionInsert(log).code(), StatusCode::kInternal);
}

TEST_F(MemoryStoreTest, SeamFailsInteractionListBySession) {
    store_->SetFailNextOps(1);
    std::vector<InteractionLog> out;
    EXPECT_EQ(store_->InteractionListBySession("s", out).code(), StatusCode::kInternal);
}

TEST_F(MemoryStoreTest, SeamFailsInteractionGetRecent) {
    store_->SetFailNextOps(1);
    std::vector<InteractionLog> out;
    EXPECT_EQ(store_->InteractionGetRecent("s", 5, out).code(), StatusCode::kInternal);
}

TEST_F(MemoryStoreTest, SeamFailsInteractionSearch) {
    store_->SetFailNextOps(1);
    std::vector<InteractionLog> out;
    // query non-empty so the seam (not the empty-query guard) is what fails it.
    EXPECT_EQ(store_->InteractionSearch("default", "q", "", "", 10, out).code(),
              StatusCode::kInternal);
}

TEST_F(MemoryStoreTest, SeamFailsInteractionCount) {
    store_->SetFailNextOps(1);
    int64_t count = -1;
    EXPECT_EQ(store_->InteractionCount("s", &count).code(), StatusCode::kInternal);
}

TEST_F(MemoryStoreTest, SeamFailsSessionCount) {
    store_->SetFailNextOps(1);
    int64_t count = -1;
    EXPECT_EQ(store_->SessionCount("default", &count).code(), StatusCode::kInternal);
}

TEST_F(MemoryStoreTest, SeamFailsSessionTouch) {
    store_->SetFailNextOps(1);
    EXPECT_EQ(store_->SessionTouch("s").code(), StatusCode::kInternal);
}

TEST_F(MemoryStoreTest, SeamFailsInteractionPairInsertAndSessionTouch) {
    InteractionLog u;
    u.session_id = "s"; u.namespace_name = "default"; u.role = "user"; u.content = "q";
    InteractionLog a;
    a.session_id = "s"; a.namespace_name = "default"; a.role = "assistant"; a.content = "r";
    store_->SetFailNextOps(1);
    EXPECT_EQ(store_->InteractionPairInsertAndSessionTouch(u, a, "s").code(),
              StatusCode::kInternal);
}

TEST_F(MemoryStoreTest, SeamFailsSessionGetOptOut) {
    store_->SetFailNextOps(1);
    bool exists = true;
    std::string at, by;
    EXPECT_EQ(store_->SessionGetOptOut("s", &exists, &at, &by).code(),
              StatusCode::kInternal);
}

TEST_F(MemoryStoreTest, SeamFailsSessionSetOptOut) {
    store_->SetFailNextOps(1);
    EXPECT_EQ(store_->SessionSetOptOut("s", "2026-05-16T10:30:00.000Z", "test").code(),
              StatusCode::kInternal);
}

TEST_F(MemoryStoreTest, SeamFailsSessionClearOptOut) {
    store_->SetFailNextOps(1);
    EXPECT_EQ(store_->SessionClearOptOut("s").code(), StatusCode::kInternal);
}

// SetFailNextOps is a countdown: arming N makes exactly the next N calls fail, then
// normal operation resumes (covers the consume/decrement branch in TryConsumeOpFault).
TEST_F(MemoryStoreTest, SeamCountdownConsumesExactlyNCalls) {
    store_->SetFailNextOps(2);
    int64_t count = -1;
    EXPECT_EQ(store_->SessionCount("default", &count).code(), StatusCode::kInternal);
    EXPECT_EQ(store_->SessionCount("default", &count).code(), StatusCode::kInternal);
    auto s = store_->SessionCount("default", &count);
    EXPECT_TRUE(s.ok()) << s.message();
}

// --- Search with both session and user filters ---

TEST_F(MemoryStoreTest, InteractionSearchBothFilters) {
    MemorySession session;
    session.namespace_name = "default";
    store_->SessionCreate(session);

    InteractionLog log;
    log.session_id = session.session_id;
    log.namespace_name = "default";
    log.user_id = "specific_user";
    log.role = "user";
    log.content = "both filters content";
    store_->InteractionInsert(log);

    std::vector<InteractionLog> results;
    auto s = store_->InteractionSearch("default", "both filters",
                                       session.session_id, "specific_user",
                                       10, results);
    ASSERT_TRUE(s.ok());
    EXPECT_EQ(results.size(), 1u);
}

}  // namespace
}  // namespace cortrix
