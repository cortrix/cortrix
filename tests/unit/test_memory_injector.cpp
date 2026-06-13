#include <gtest/gtest.h>
#include "cortrix/memory/memory_injector.h"
#include "cortrix/memory/memory_store.h"

namespace cortrix {
namespace {

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

class MemoryInjectorTest : public ::testing::Test {
protected:
    void SetUp() override {
        mem_store_ = std::make_unique<MemoryStore>(null_store_);
        auto s = mem_store_->Init(":memory:");
        ASSERT_TRUE(s.ok());

        config_.inject_recent_turns = 3;
        config_.inject_max_tokens = 2000;
    }

    // Insert N turns of user/assistant pairs
    void InsertTurns(const std::string& session_id, int num_turns) {
        for (int i = 0; i < num_turns; i++) {
            InteractionLog user_log;
            user_log.session_id = session_id;
            user_log.namespace_name = "default";
            user_log.role = "user";
            user_log.content = "Question " + std::to_string(i);
            mem_store_->InteractionInsert(user_log);

            InteractionLog asst_log;
            asst_log.session_id = session_id;
            asst_log.namespace_name = "default";
            asst_log.role = "assistant";
            asst_log.content = "Answer " + std::to_string(i);
            mem_store_->InteractionInsert(asst_log);
        }
    }

    NullCortrixStore null_store_;
    std::unique_ptr<MemoryStore> mem_store_;
    MemoryConfig config_;
};

TEST_F(MemoryInjectorTest, GetRecentBasic) {
    MemorySession session;
    session.namespace_name = "default";
    mem_store_->SessionCreate(session);
    InsertTurns(session.session_id, 5);

    MemoryInjector injector(*mem_store_, config_);
    auto ctx = injector.GetRecentContext(session.session_id);

    EXPECT_EQ(ctx.turn_count, 3);  // inject_recent_turns = 3
    EXPECT_GT(ctx.token_count_approx, 0);
    EXPECT_FALSE(ctx.context_text.empty());

    // Should contain the last 3 turns (turns 2, 3, 4) in order
    EXPECT_NE(ctx.context_text.find("Question 2"), std::string::npos);
    EXPECT_NE(ctx.context_text.find("Answer 4"), std::string::npos);

    // Should NOT contain turn 0 or 1
    EXPECT_EQ(ctx.context_text.find("Question 0"), std::string::npos);
    EXPECT_EQ(ctx.context_text.find("Question 1"), std::string::npos);
}

TEST_F(MemoryInjectorTest, GetRecentEmpty) {
    MemorySession session;
    session.namespace_name = "default";
    mem_store_->SessionCreate(session);

    MemoryInjector injector(*mem_store_, config_);
    auto ctx = injector.GetRecentContext(session.session_id);

    EXPECT_EQ(ctx.turn_count, 0);
    EXPECT_EQ(ctx.token_count_approx, 0);
    EXPECT_TRUE(ctx.context_text.empty());
}

TEST_F(MemoryInjectorTest, GetRecentSingleTurn) {
    MemorySession session;
    session.namespace_name = "default";
    mem_store_->SessionCreate(session);
    InsertTurns(session.session_id, 1);

    MemoryInjector injector(*mem_store_, config_);
    auto ctx = injector.GetRecentContext(session.session_id);

    EXPECT_EQ(ctx.turn_count, 1);
    EXPECT_NE(ctx.context_text.find("Question 0"), std::string::npos);
    EXPECT_NE(ctx.context_text.find("Answer 0"), std::string::npos);
}

TEST_F(MemoryInjectorTest, FormatCorrect) {
    MemorySession session;
    session.namespace_name = "default";
    mem_store_->SessionCreate(session);
    InsertTurns(session.session_id, 1);

    MemoryInjector injector(*mem_store_, config_);
    auto ctx = injector.GetRecentContext(session.session_id);

    // Format: "User: ...\nAssistant: ...\n\n"
    EXPECT_NE(ctx.context_text.find("User: Question 0\n"), std::string::npos);
    EXPECT_NE(ctx.context_text.find("Assistant: Answer 0\n"), std::string::npos);
}

TEST_F(MemoryInjectorTest, GetRecentTokenLimit) {
    MemorySession session;
    session.namespace_name = "default";
    mem_store_->SessionCreate(session);

    // Insert 10 turns
    InsertTurns(session.session_id, 10);

    // Set very low token limit
    config_.inject_recent_turns = 10;
    config_.inject_max_tokens = 30;  // very low

    MemoryInjector injector(*mem_store_, config_);
    auto ctx = injector.GetRecentContext(session.session_id);

    // Should be truncated
    EXPECT_LT(ctx.turn_count, 10);
    EXPECT_LE(ctx.token_count_approx, 30);
}

TEST_F(MemoryInjectorTest, ChineseContent) {
    MemorySession session;
    session.namespace_name = "default";
    mem_store_->SessionCreate(session);

    InteractionLog user_log;
    user_log.session_id = session.session_id;
    user_log.namespace_name = "default";
    user_log.role = "user";
    user_log.content = "hello world";
    mem_store_->InteractionInsert(user_log);

    InteractionLog asst_log;
    asst_log.session_id = session.session_id;
    asst_log.namespace_name = "default";
    asst_log.role = "assistant";
    asst_log.content = "world hello";
    mem_store_->InteractionInsert(asst_log);

    MemoryInjector injector(*mem_store_, config_);
    auto ctx = injector.GetRecentContext(session.session_id);

    EXPECT_EQ(ctx.turn_count, 1);
    EXPECT_NE(ctx.context_text.find("hello world"), std::string::npos);
    EXPECT_NE(ctx.context_text.find("world hello"), std::string::npos);
    EXPECT_GT(ctx.token_count_approx, 0);
}

// --- Non-existent session (empty result from InteractionGetRecent) ---

TEST_F(MemoryInjectorTest, GetRecentNonexistentSession) {
    MemoryInjector injector(*mem_store_, config_);
    auto ctx = injector.GetRecentContext("nonexistent-session-id");

    EXPECT_EQ(ctx.turn_count, 0);
    EXPECT_EQ(ctx.token_count_approx, 0);
    EXPECT_TRUE(ctx.context_text.empty());
}

// --- Unpaired messages (user without assistant) ---

TEST_F(MemoryInjectorTest, UnpairedUserMessage) {
    MemorySession session;
    session.namespace_name = "default";
    mem_store_->SessionCreate(session);

    // Insert only user messages (no assistant pairs)
    for (int i = 0; i < 3; i++) {
        InteractionLog log;
        log.session_id = session.session_id;
        log.namespace_name = "default";
        log.role = "user";
        log.content = "Orphan question " + std::to_string(i);
        mem_store_->InteractionInsert(log);
    }

    MemoryInjector injector(*mem_store_, config_);
    auto ctx = injector.GetRecentContext(session.session_id);

    // No pairs formed, so no turns
    EXPECT_EQ(ctx.turn_count, 0);
    EXPECT_TRUE(ctx.context_text.empty());
}

// --- Assistant-only messages ---

TEST_F(MemoryInjectorTest, AssistantOnlyMessages) {
    MemorySession session;
    session.namespace_name = "default";
    mem_store_->SessionCreate(session);

    // Insert only assistant messages
    for (int i = 0; i < 3; i++) {
        InteractionLog log;
        log.session_id = session.session_id;
        log.namespace_name = "default";
        log.role = "assistant";
        log.content = "Orphan answer " + std::to_string(i);
        mem_store_->InteractionInsert(log);
    }

    MemoryInjector injector(*mem_store_, config_);
    auto ctx = injector.GetRecentContext(session.session_id);

    // No user messages to start pairs, so no turns
    EXPECT_EQ(ctx.turn_count, 0);
    EXPECT_TRUE(ctx.context_text.empty());
}

// --- Mixed: user, assistant, user (second user without assistant) ---

TEST_F(MemoryInjectorTest, MixedWithUnpairedTail) {
    MemorySession session;
    session.namespace_name = "default";
    mem_store_->SessionCreate(session);

    // user, assistant, user (last user has no pair)
    InteractionLog log1;
    log1.session_id = session.session_id;
    log1.namespace_name = "default";
    log1.role = "user";
    log1.content = "Q1";
    mem_store_->InteractionInsert(log1);

    InteractionLog log2;
    log2.session_id = session.session_id;
    log2.namespace_name = "default";
    log2.role = "assistant";
    log2.content = "A1";
    mem_store_->InteractionInsert(log2);

    InteractionLog log3;
    log3.session_id = session.session_id;
    log3.namespace_name = "default";
    log3.role = "user";
    log3.content = "Q2 orphan";
    mem_store_->InteractionInsert(log3);

    MemoryInjector injector(*mem_store_, config_);
    auto ctx = injector.GetRecentContext(session.session_id);

    EXPECT_EQ(ctx.turn_count, 1);  // only Q1+A1 pair
    EXPECT_NE(ctx.context_text.find("Q1"), std::string::npos);
    EXPECT_NE(ctx.context_text.find("A1"), std::string::npos);
    EXPECT_EQ(ctx.context_text.find("Q2 orphan"), std::string::npos);
}

// --- ApproxTokenCount edge cases ---

TEST_F(MemoryInjectorTest, EmptyStringTokenCount) {
    // ApproxTokenCount is private, but we test through context behavior
    MemorySession session;
    session.namespace_name = "default";
    mem_store_->SessionCreate(session);

    // Insert user/assistant pair with empty-ish content
    InteractionLog user_log;
    user_log.session_id = session.session_id;
    user_log.namespace_name = "default";
    user_log.role = "user";
    user_log.content = "a";
    mem_store_->InteractionInsert(user_log);

    InteractionLog asst_log;
    asst_log.session_id = session.session_id;
    asst_log.namespace_name = "default";
    asst_log.role = "assistant";
    asst_log.content = "b";
    mem_store_->InteractionInsert(asst_log);

    MemoryInjector injector(*mem_store_, config_);
    auto ctx = injector.GetRecentContext(session.session_id);

    EXPECT_EQ(ctx.turn_count, 1);
    EXPECT_GT(ctx.token_count_approx, 0);
}

// --- UTF-8 4-byte characters ---

TEST_F(MemoryInjectorTest, FourByteUTF8Content) {
    MemorySession session;
    session.namespace_name = "default";
    mem_store_->SessionCreate(session);

    // Emoji (4-byte UTF-8)
    InteractionLog user_log;
    user_log.session_id = session.session_id;
    user_log.namespace_name = "default";
    user_log.role = "user";
    user_log.content = "\xF0\x9F\x98\x80 hello";  // emoji + text
    mem_store_->InteractionInsert(user_log);

    InteractionLog asst_log;
    asst_log.session_id = session.session_id;
    asst_log.namespace_name = "default";
    asst_log.role = "assistant";
    asst_log.content = "reply \xF0\x9F\x91\x8D";  // text + emoji
    mem_store_->InteractionInsert(asst_log);

    MemoryInjector injector(*mem_store_, config_);
    auto ctx = injector.GetRecentContext(session.session_id);

    EXPECT_EQ(ctx.turn_count, 1);
    EXPECT_GT(ctx.token_count_approx, 0);
}

// --- Exact token limit boundary ---

TEST_F(MemoryInjectorTest, TokenLimitExactBoundary) {
    MemorySession session;
    session.namespace_name = "default";
    mem_store_->SessionCreate(session);

    // Insert many turns
    InsertTurns(session.session_id, 20);

    // Set token limit to something very small
    config_.inject_recent_turns = 20;
    config_.inject_max_tokens = 1;

    MemoryInjector injector(*mem_store_, config_);
    auto ctx = injector.GetRecentContext(session.session_id);

    // Should not exceed token limit
    EXPECT_LE(ctx.token_count_approx, 1);
    // Might be 0 turns if even one turn exceeds the limit
    EXPECT_LE(ctx.turn_count, 1);
}

// --- System role messages should be skipped ---

TEST_F(MemoryInjectorTest, SystemMessagesSkipped) {
    MemorySession session;
    session.namespace_name = "default";
    mem_store_->SessionCreate(session);

    // system, user, assistant pattern
    InteractionLog sys_log;
    sys_log.session_id = session.session_id;
    sys_log.namespace_name = "default";
    sys_log.role = "system";
    sys_log.content = "You are a helpful assistant";
    mem_store_->InteractionInsert(sys_log);

    InsertTurns(session.session_id, 1);

    MemoryInjector injector(*mem_store_, config_);
    auto ctx = injector.GetRecentContext(session.session_id);

    EXPECT_EQ(ctx.turn_count, 1);
    // System message should not appear in context
    EXPECT_EQ(ctx.context_text.find("You are a helpful assistant"), std::string::npos);
}

}  // namespace
}  // namespace cortrix
