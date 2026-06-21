#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "cortrix/memory/memory_store.h"

// DEPTH — InteractionSearch with CJK / unicode queries. The existing
// test_memory_store.cpp exercises the LIKE-escape path only for ASCII wildcards
// (%, _, backslash) and has ZERO unicode coverage. SQLite's default LIKE is
// byte-wise (no Unicode case folding), and EscapeLikePattern() escapes per-byte;
// a multi-byte UTF-8 CJG codepoint never contains the 0x25/0x5F/0x5C escape
// bytes in its continuation bytes, so a CJK substring must match literally and a
// CJK query carrying a wildcard char must still be escaped. These tests pin that.
// Suite name is globally unique (MemStoreUnicodeDepth).
namespace cortrix {
namespace {

// Minimal CortrixStore stub (mirrors test_memory_store.cpp NullCortrixStore):
// MemoryStore only needs CortrixStore& for doc ops; interaction_log uses its own
// SQLite connection.
class UnicodeNullCortrixStore : public CortrixStore {
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

// CJK literals via \x UTF-8 escapes (ASCII-only source per repo policy):
//   kZhBeijing = "Beijing" (Beijing)
//   kZhShanghai= "Shanghai" (Shanghai)
//   kZhWeather = "weather" (weather)
const std::string kZhBeijing  = "\xE5\x8C\x97\xE4\xBA\xAC";          // Beijing
const std::string kZhShanghai = "\xE4\xB8\x8A\xE6\xB5\xB7";          // Shanghai
const std::string kZhWeather  = "\xE5\xA4\xA9\xE6\xB0\x94";          // weather
const std::string kZhUserInBeijing =
    "\xE7\x94\xA8\xE6\x88\xB7\xE5\x9C\xA8" + kZhBeijing;             // user in Beijing

class MemStoreUnicodeDepthTest : public ::testing::Test {
protected:
    void SetUp() override {
        store_ = std::make_unique<MemoryStore>(null_store_);
        ASSERT_TRUE(store_->Init(":memory:").ok());
    }

    // Insert an interaction with the given content into the default namespace.
    std::string Insert(const std::string& content, const std::string& sid = "sess1",
                       const std::string& uid = "") {
        InteractionLog log;
        log.session_id = sid;
        log.namespace_name = "default";
        log.user_id = uid;
        log.role = "user";
        log.content = content;
        EXPECT_TRUE(store_->InteractionInsert(log).ok());
        return log.id;
    }

    UnicodeNullCortrixStore null_store_;
    std::unique_ptr<MemoryStore> store_;
};

TEST_F(MemStoreUnicodeDepthTest, CjkSubstringMatches) {
    Insert(kZhUserInBeijing);                 // user in Beijing
    Insert("user is in " + kZhShanghai);      // mixed ascii + Shanghai
    std::vector<InteractionLog> results;
    auto s = store_->InteractionSearch("default", kZhBeijing, "", "", 10, results);
    ASSERT_TRUE(s.ok()) << s.message();
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].content, kZhUserInBeijing);
}

TEST_F(MemStoreUnicodeDepthTest, CjkQueryNoMatchReturnsEmpty) {
    Insert(kZhUserInBeijing);
    std::vector<InteractionLog> results;
    auto s = store_->InteractionSearch("default", kZhWeather, "", "", 10, results);
    ASSERT_TRUE(s.ok());
    EXPECT_TRUE(results.empty());
}

TEST_F(MemStoreUnicodeDepthTest, MixedAsciiCjkSubstringMatches) {
    Insert("user is in " + kZhShanghai + " today");
    std::vector<InteractionLog> results;
    auto s = store_->InteractionSearch("default", kZhShanghai, "", "", 10, results);
    ASSERT_TRUE(s.ok());
    ASSERT_EQ(results.size(), 1u);
}

TEST_F(MemStoreUnicodeDepthTest, CjkQueryWithLiteralPercentIsEscaped) {
    // CJK text adjacent to a literal '%' — the '%' must be escaped so it is a
    // literal, not a wildcard. A row WITHOUT the literal % must not match.
    Insert(kZhBeijing + "100%");              // Beijing100%
    Insert(kZhBeijing + "200");               // Beijing200 (no percent)
    std::vector<InteractionLog> results;
    auto s = store_->InteractionSearch("default", kZhBeijing + "100%", "", "", 10, results);
    ASSERT_TRUE(s.ok());
    ASSERT_EQ(results.size(), 1u);
    EXPECT_NE(results[0].content.find("100%"), std::string::npos);
}

TEST_F(MemStoreUnicodeDepthTest, CjkQueryWithLiteralUnderscoreIsEscaped) {
    Insert(kZhWeather + "_log");              // weather_log
    Insert(kZhWeather + "Xlog");              // weatherXlog (underscore-as-wildcard would match)
    std::vector<InteractionLog> results;
    auto s = store_->InteractionSearch("default", kZhWeather + "_log", "", "", 10, results);
    ASSERT_TRUE(s.ok());
    // Escaped '_' is literal → only the exact "weather_log" row matches, not "weatherXlog".
    ASSERT_EQ(results.size(), 1u);
    EXPECT_NE(results[0].content.find("_log"), std::string::npos);
}

TEST_F(MemStoreUnicodeDepthTest, CjkSearchRespectsSessionFilter) {
    Insert(kZhBeijing, "sess_a");
    Insert(kZhBeijing, "sess_b");
    std::vector<InteractionLog> results;
    auto s = store_->InteractionSearch("default", kZhBeijing, "sess_a", "", 10, results);
    ASSERT_TRUE(s.ok());
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].session_id, "sess_a");
}

TEST_F(MemStoreUnicodeDepthTest, CjkSearchRespectsUserFilter) {
    Insert(kZhShanghai, "sess1", "userA");
    Insert(kZhShanghai, "sess1", "userB");
    std::vector<InteractionLog> results;
    auto s = store_->InteractionSearch("default", kZhShanghai, "", "userA", 10, results);
    ASSERT_TRUE(s.ok());
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].user_id, "userA");
}

TEST_F(MemStoreUnicodeDepthTest, CjkSearchNamespaceIsolation) {
    Insert(kZhBeijing);  // in "default"
    InteractionLog other;
    other.session_id = "sess_o";
    other.namespace_name = "ns_other";
    other.role = "user";
    other.content = kZhBeijing;
    ASSERT_TRUE(store_->InteractionInsert(other).ok());

    std::vector<InteractionLog> results;
    auto s = store_->InteractionSearch("ns_other", kZhBeijing, "", "", 10, results);
    ASSERT_TRUE(s.ok());
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].namespace_name, "ns_other");
}

TEST_F(MemStoreUnicodeDepthTest, CjkQueryWithBackslashIsEscaped) {
    // A backslash adjacent to CJK must be escaped (ESCAPE '\') so it is literal.
    Insert(kZhWeather + "\\path");            // weather\path
    std::vector<InteractionLog> results;
    auto s = store_->InteractionSearch("default", kZhWeather + "\\path", "", "", 10, results);
    ASSERT_TRUE(s.ok());
    ASSERT_EQ(results.size(), 1u);
}

TEST_F(MemStoreUnicodeDepthTest, MultibyteQueryDoesNotCorruptResultContent) {
    // Round-trip integrity: a 4-byte emoji codepoint stored + searched returns the
    // exact bytes (no truncation at the multi-byte boundary).
    const std::string kRocket = "\xF0\x9F\x9A\x80";  // U+1F680 ROCKET
    Insert("launch " + kRocket + " now");
    std::vector<InteractionLog> results;
    auto s = store_->InteractionSearch("default", kRocket, "", "", 10, results);
    ASSERT_TRUE(s.ok());
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].content, "launch " + kRocket + " now");
}

}  // namespace
}  // namespace cortrix
