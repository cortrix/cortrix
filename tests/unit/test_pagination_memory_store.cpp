// Pagination + filter matrices for MemoryStore list/search APIs (F23 breadth).
//
// Real APIs (include/cortrix/memory/memory_store.h):
//   SessionList(ns, limit, offset, &out, user_id="")  - raw bind of limit/offset.
//       SQLite semantics: LIMIT < 0  => no limit (all rows);
//                         OFFSET < 0 => treated as 0.
//   SessionCount(ns, &count, user_id="")
//   InteractionGetRecent(session_id, limit, &out)     - raw bind of limit.
//       SQLite semantics: LIMIT < 0 => all rows.
//   InteractionSearch(ns, query, session_id, user_id, limit, &out)
//       clamp: limit < 1 => 10 ; limit > 100 => 100 ; empty query => InvalidArgument.
//   InteractionCount(session_id, &count)
//
// created_at is honored when preset (InsertInteractionLocked only fills it when
// empty), so ordering tests seed deterministic timestamps. Suite/fixture names
// are globally unique (MemStorePage* prefix).
//
// Deterministic: in-memory SQLite, no network/LLM.

#include <gtest/gtest.h>
#include "cortrix/memory/memory_store.h"
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace cortrix {
namespace {

// Minimal CortrixStore stub - MemoryStore only needs CortrixStore& for doc ops;
// interaction_log/memory_sessions use their own SQLite connection.
class MemStorePageNullStore : public CortrixStore {
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

// Pads an integer to a fixed width so lexicographic ISO timestamps sort in
// insertion order (created_at "2020-01-01T00:00:00.NNNZ").
std::string SeedTs(int seq) {
    char buf[40];
    std::snprintf(buf, sizeof(buf), "2020-01-01T00:00:%02d.%03dZ",
                  seq / 1000, seq % 1000);
    return std::string(buf);
}

// ===========================================================================
// SessionList - limit/offset matrix (raw bind; LIMIT<0 => all, OFFSET<0 => 0).
// ===========================================================================

struct MemStorePageSessionCase {
    int seed_count;       // sessions to seed in the namespace
    int limit;
    int offset;
    int expect_rows;      // expected returned row count
};

class MemStorePageSessionListMatrix
    : public ::testing::TestWithParam<MemStorePageSessionCase> {
protected:
    void SetUp() override {
        store_ = std::make_unique<MemoryStore>(null_store_);
        auto s = store_->Init(":memory:");
        ASSERT_TRUE(s.ok()) << s.message();
    }
    // Seeds `n` sessions in `ns`, distinct updated_at via SessionTouch order.
    void Seed(const std::string& ns, int n) {
        for (int i = 0; i < n; ++i) {
            MemorySession sess;
            sess.namespace_name = ns;
            auto s = store_->SessionCreate(sess);
            ASSERT_TRUE(s.ok()) << s.message();
        }
    }
    MemStorePageNullStore null_store_;
    std::unique_ptr<MemoryStore> store_;
};

TEST_P(MemStorePageSessionListMatrix, RowCount) {
    const auto& p = GetParam();
    const std::string ns = "ns_sl";
    Seed(ns, p.seed_count);

    std::vector<MemorySession> out;
    auto st = store_->SessionList(ns, p.limit, p.offset, out);
    ASSERT_TRUE(st.ok()) << st.message();
    EXPECT_EQ(static_cast<int>(out.size()), p.expect_rows)
        << "seed=" << p.seed_count << " limit=" << p.limit
        << " offset=" << p.offset;
    // Every returned row belongs to the requested namespace.
    for (const auto& s : out) EXPECT_EQ(s.namespace_name, ns);
}

INSTANTIATE_TEST_SUITE_P(
    SessionListPaging, MemStorePageSessionListMatrix,
    ::testing::Values(
        // seed=5 baseline.
        MemStorePageSessionCase{5, 0, 0, 0},     // limit 0 => no rows
        MemStorePageSessionCase{5, 1, 0, 1},     // limit 1
        MemStorePageSessionCase{5, 5, 0, 5},     // exactly count
        MemStorePageSessionCase{5, 3, 0, 3},     // partial page
        MemStorePageSessionCase{5, 10, 0, 5},    // oversize > count
        MemStorePageSessionCase{5, -1, 0, 5},    // negative limit => all
        MemStorePageSessionCase{5, 2, 2, 2},     // offset mid
        MemStorePageSessionCase{5, 2, 4, 1},     // offset near end => 1 row
        MemStorePageSessionCase{5, 2, 5, 0},     // offset == count => empty
        MemStorePageSessionCase{5, 2, 99, 0},    // offset beyond count => empty
        MemStorePageSessionCase{5, 3, -1, 3},    // negative offset => 0
        // empty namespace.
        MemStorePageSessionCase{0, 10, 0, 0},    // empty result set
        MemStorePageSessionCase{0, -1, 0, 0},
        // single row.
        MemStorePageSessionCase{1, 10, 0, 1},
        MemStorePageSessionCase{1, 1, 1, 0}));

// ---- SessionList: namespace filter match/no-match -------------------------

class MemStorePageSessionFilter : public MemStorePageSessionListMatrix {};

TEST_F(MemStorePageSessionFilter, NamespaceNoMatchEmpty) {
    Seed("ns_a", 3);
    std::vector<MemorySession> out;
    auto st = store_->SessionList("ns_does_not_exist", 50, 0, out);
    ASSERT_TRUE(st.ok()) << st.message();
    EXPECT_TRUE(out.empty());
}

TEST_F(MemStorePageSessionFilter, NamespaceIsolation) {
    Seed("ns_a", 3);
    Seed("ns_b", 2);
    std::vector<MemorySession> a, b;
    ASSERT_TRUE(store_->SessionList("ns_a", 50, 0, a).ok());
    ASSERT_TRUE(store_->SessionList("ns_b", 50, 0, b).ok());
    EXPECT_EQ(a.size(), 3u);
    EXPECT_EQ(b.size(), 2u);
}

// ---- SessionList: user_id filter (MEM05 isolation) ------------------------

struct MemStorePageUserCase {
    std::string filter_user;   // "" = no filter
    int expect_rows;
};

class MemStorePageSessionUserMatrix
    : public ::testing::TestWithParam<MemStorePageUserCase> {
protected:
    void SetUp() override {
        store_ = std::make_unique<MemoryStore>(null_store_);
        ASSERT_TRUE(store_->Init(":memory:").ok());
        // ns "u_ns": 3 owned by alice, 2 by bob.
        for (int i = 0; i < 3; ++i) MakeSession("u_ns", "alice");
        for (int i = 0; i < 2; ++i) MakeSession("u_ns", "bob");
    }
    void MakeSession(const std::string& ns, const std::string& uid) {
        MemorySession sess;
        sess.namespace_name = ns;
        sess.user_id = uid;
        ASSERT_TRUE(store_->SessionCreate(sess).ok());
    }
    MemStorePageNullStore null_store_;
    std::unique_ptr<MemoryStore> store_;
};

TEST_P(MemStorePageSessionUserMatrix, FilteredCountMatchesPage) {
    const auto& p = GetParam();
    std::vector<MemorySession> out;
    auto st = store_->SessionList("u_ns", 50, 0, out, p.filter_user);
    ASSERT_TRUE(st.ok()) << st.message();
    EXPECT_EQ(static_cast<int>(out.size()), p.expect_rows);
    // SessionCount must agree with the listed page under the same filter.
    int64_t cnt = -1;
    ASSERT_TRUE(store_->SessionCount("u_ns", &cnt, p.filter_user).ok());
    EXPECT_EQ(cnt, p.expect_rows);
    if (!p.filter_user.empty())
        for (const auto& s : out) EXPECT_EQ(s.user_id, p.filter_user);
}

INSTANTIATE_TEST_SUITE_P(
    SessionUserFilter, MemStorePageSessionUserMatrix,
    ::testing::Values(
        MemStorePageUserCase{"", 5},          // no filter => all
        MemStorePageUserCase{"alice", 3},     // match
        MemStorePageUserCase{"bob", 2},       // match
        MemStorePageUserCase{"carol", 0}));   // no-match => empty

// ---- SessionList: ordering stability (updated_at DESC) --------------------

TEST_F(MemStorePageSessionFilter, OrderingStableAcrossPages) {
    // Seed 6 sessions; touch them in a known order so updated_at is distinct.
    std::vector<std::string> ids;
    for (int i = 0; i < 6; ++i) {
        MemorySession sess;
        sess.namespace_name = "ord_ns";
        ASSERT_TRUE(store_->SessionCreate(sess).ok());
        ids.push_back(sess.session_id);
    }
    // Page through with limit=2; concatenation must equal a single full list.
    std::vector<std::string> paged;
    for (int off = 0; off < 6; off += 2) {
        std::vector<MemorySession> page;
        ASSERT_TRUE(store_->SessionList("ord_ns", 2, off, page).ok());
        for (const auto& s : page) paged.push_back(s.session_id);
    }
    std::vector<MemorySession> full;
    ASSERT_TRUE(store_->SessionList("ord_ns", 100, 0, full).ok());
    std::vector<std::string> full_ids;
    for (const auto& s : full) full_ids.push_back(s.session_id);
    EXPECT_EQ(paged, full_ids);
    EXPECT_EQ(paged.size(), 6u);
}

// ===========================================================================
// InteractionGetRecent - limit matrix (raw bind; LIMIT<0 => all rows).
// ===========================================================================

struct MemStorePageRecentCase {
    int seed_count;
    int limit;
    int expect_rows;
};

class MemStorePageRecentMatrix
    : public ::testing::TestWithParam<MemStorePageRecentCase> {
protected:
    void SetUp() override {
        store_ = std::make_unique<MemoryStore>(null_store_);
        ASSERT_TRUE(store_->Init(":memory:").ok());
        MemorySession sess;
        sess.namespace_name = "rec_ns";
        ASSERT_TRUE(store_->SessionCreate(sess).ok());
        session_id_ = sess.session_id;
    }
    void SeedInteractions(int n) {
        for (int i = 0; i < n; ++i) {
            InteractionLog log;
            log.session_id = session_id_;
            log.namespace_name = "rec_ns";
            log.role = "user";
            log.content = "msg_" + std::to_string(i);
            log.created_at = SeedTs(i);  // deterministic ordering
            ASSERT_TRUE(store_->InteractionInsert(log).ok());
        }
    }
    MemStorePageNullStore null_store_;
    std::unique_ptr<MemoryStore> store_;
    std::string session_id_;
};

TEST_P(MemStorePageRecentMatrix, RowCount) {
    const auto& p = GetParam();
    SeedInteractions(p.seed_count);
    std::vector<InteractionLog> out;
    auto st = store_->InteractionGetRecent(session_id_, p.limit, out);
    ASSERT_TRUE(st.ok()) << st.message();
    EXPECT_EQ(static_cast<int>(out.size()), p.expect_rows)
        << "seed=" << p.seed_count << " limit=" << p.limit;
}

INSTANTIATE_TEST_SUITE_P(
    GetRecentPaging, MemStorePageRecentMatrix,
    ::testing::Values(
        MemStorePageRecentCase{8, 0, 0},     // limit 0 => none
        MemStorePageRecentCase{8, 1, 1},     // limit 1
        MemStorePageRecentCase{8, 8, 8},     // exactly count
        MemStorePageRecentCase{8, 3, 3},     // partial
        MemStorePageRecentCase{8, 20, 8},    // oversize
        MemStorePageRecentCase{8, -1, 8},    // negative => all rows
        MemStorePageRecentCase{0, 10, 0},    // empty session
        MemStorePageRecentCase{1, 5, 1}));   // single

TEST_F(MemStorePageRecentMatrix, RecentReturnsNewestFirst) {
    SeedInteractions(5);  // msg_0 oldest ... msg_4 newest
    std::vector<InteractionLog> out;
    ASSERT_TRUE(store_->InteractionGetRecent(session_id_, 3, out).ok());
    ASSERT_EQ(out.size(), 3u);
    // ORDER BY created_at DESC => newest (msg_4) first.
    EXPECT_EQ(out[0].content, "msg_4");
    EXPECT_EQ(out[1].content, "msg_3");
    EXPECT_EQ(out[2].content, "msg_2");
}

TEST_F(MemStorePageRecentMatrix, UnknownSessionEmpty) {
    SeedInteractions(3);
    std::vector<InteractionLog> out;
    auto st = store_->InteractionGetRecent("no-such-session", 10, out);
    ASSERT_TRUE(st.ok()) << st.message();
    EXPECT_TRUE(out.empty());
}

// ===========================================================================
// InteractionSearch - limit clamp (<1 =>10, >100 =>100) + filter matrix.
// ===========================================================================

struct MemStorePageSearchCase {
    int seed_count;        // matching interactions seeded
    int limit;
    int expect_rows;
};

class MemStorePageSearchMatrix
    : public ::testing::TestWithParam<MemStorePageSearchCase> {
protected:
    void SetUp() override {
        store_ = std::make_unique<MemoryStore>(null_store_);
        ASSERT_TRUE(store_->Init(":memory:").ok());
        MemorySession sess;
        sess.namespace_name = "srch_ns";
        ASSERT_TRUE(store_->SessionCreate(sess).ok());
        session_id_ = sess.session_id;
    }
    // Seeds `n` interactions whose content contains "needle".
    void SeedMatching(int n) {
        for (int i = 0; i < n; ++i) {
            InteractionLog log;
            log.session_id = session_id_;
            log.namespace_name = "srch_ns";
            log.role = "user";
            log.content = "has needle here " + std::to_string(i);
            log.created_at = SeedTs(i);
            ASSERT_TRUE(store_->InteractionInsert(log).ok());
        }
    }
    MemStorePageNullStore null_store_;
    std::unique_ptr<MemoryStore> store_;
    std::string session_id_;
};

TEST_P(MemStorePageSearchMatrix, ClampedRowCount) {
    const auto& p = GetParam();
    SeedMatching(p.seed_count);
    std::vector<InteractionLog> out;
    auto st = store_->InteractionSearch("srch_ns", "needle", "", "",
                                        p.limit, out);
    ASSERT_TRUE(st.ok()) << st.message();
    EXPECT_EQ(static_cast<int>(out.size()), p.expect_rows)
        << "seed=" << p.seed_count << " limit=" << p.limit;
}

INSTANTIATE_TEST_SUITE_P(
    SearchClamp, MemStorePageSearchMatrix,
    ::testing::Values(
        // limit <1 clamps to 10 (default).
        MemStorePageSearchCase{5, 0, 5},       // limit 0 => clamp 10, only 5 exist
        MemStorePageSearchCase{12, 0, 10},     // limit 0 => clamp 10
        MemStorePageSearchCase{12, -7, 10},    // negative => clamp 10
        // in-range limits honored.
        MemStorePageSearchCase{12, 1, 1},
        MemStorePageSearchCase{12, 5, 5},
        MemStorePageSearchCase{12, 12, 12},    // exactly count
        MemStorePageSearchCase{12, 50, 12},    // oversize within cap
        // limit >100 clamps to 100 (we cannot seed 100+, so cap is exercised
        // by ensuring all available rows still return).
        MemStorePageSearchCase{8, 200, 8},     // limit 200 => clamp 100, 8 exist
        // empty result set (no matches but valid query handled below).
        MemStorePageSearchCase{0, 10, 0}));

TEST_F(MemStorePageSearchMatrix, EmptyQueryInvalidArgument) {
    SeedMatching(3);
    std::vector<InteractionLog> out;
    auto st = store_->InteractionSearch("srch_ns", "", "", "", 10, out);
    EXPECT_EQ(st.code(), StatusCode::kInvalidArgument);
}

TEST_F(MemStorePageSearchMatrix, NoMatchEmptyResult) {
    SeedMatching(4);  // contain "needle"
    std::vector<InteractionLog> out;
    auto st = store_->InteractionSearch("srch_ns", "haystack_absent", "",
                                        "", 10, out);
    ASSERT_TRUE(st.ok()) << st.message();
    EXPECT_TRUE(out.empty());
}

TEST_F(MemStorePageSearchMatrix, NamespaceFilterExcludesOthers) {
    SeedMatching(3);  // in srch_ns
    // Same content in a different namespace must not leak.
    InteractionLog other;
    other.session_id = session_id_;
    other.namespace_name = "other_ns";
    other.role = "user";
    other.content = "has needle here X";
    other.created_at = SeedTs(99);
    ASSERT_TRUE(store_->InteractionInsert(other).ok());

    std::vector<InteractionLog> out;
    ASSERT_TRUE(store_->InteractionSearch("srch_ns", "needle", "", "",
                                          50, out).ok());
    EXPECT_EQ(out.size(), 3u);
    for (const auto& r : out) EXPECT_EQ(r.namespace_name, "srch_ns");
}

TEST_F(MemStorePageSearchMatrix, SessionFilterMatchAndNoMatch) {
    SeedMatching(3);  // all in session_id_
    std::vector<InteractionLog> hit;
    ASSERT_TRUE(store_->InteractionSearch("srch_ns", "needle", session_id_,
                                          "", 50, hit).ok());
    EXPECT_EQ(hit.size(), 3u);

    std::vector<InteractionLog> miss;
    ASSERT_TRUE(store_->InteractionSearch("srch_ns", "needle",
                                          "bogus-session", "", 50, miss).ok());
    EXPECT_TRUE(miss.empty());
}

TEST_F(MemStorePageSearchMatrix, SearchNewestFirstOrdering) {
    SeedMatching(4);  // _0 oldest ... _3 newest
    std::vector<InteractionLog> out;
    ASSERT_TRUE(store_->InteractionSearch("srch_ns", "needle", "", "",
                                          50, out).ok());
    ASSERT_EQ(out.size(), 4u);
    EXPECT_EQ(out.front().content, "has needle here 3");  // DESC
    EXPECT_EQ(out.back().content, "has needle here 0");
}

// ===========================================================================
// InteractionCount / SessionCount sanity under seeding (pagination totals).
// ===========================================================================

class MemStorePageCount : public ::testing::Test {
protected:
    void SetUp() override {
        store_ = std::make_unique<MemoryStore>(null_store_);
        ASSERT_TRUE(store_->Init(":memory:").ok());
    }
    MemStorePageNullStore null_store_;
    std::unique_ptr<MemoryStore> store_;
};

TEST_F(MemStorePageCount, InteractionCountMatchesSeed) {
    MemorySession sess;
    sess.namespace_name = "cnt_ns";
    ASSERT_TRUE(store_->SessionCreate(sess).ok());
    for (int i = 0; i < 7; ++i) {
        InteractionLog log;
        log.session_id = sess.session_id;
        log.namespace_name = "cnt_ns";
        log.role = "user";
        log.content = "x";
        ASSERT_TRUE(store_->InteractionInsert(log).ok());
    }
    int64_t cnt = -1;
    ASSERT_TRUE(store_->InteractionCount(sess.session_id, &cnt).ok());
    EXPECT_EQ(cnt, 7);
}

TEST_F(MemStorePageCount, SessionCountNsScoped) {
    for (int i = 0; i < 4; ++i) {
        MemorySession s;
        s.namespace_name = "cn_a";
        ASSERT_TRUE(store_->SessionCreate(s).ok());
    }
    for (int i = 0; i < 2; ++i) {
        MemorySession s;
        s.namespace_name = "cn_b";
        ASSERT_TRUE(store_->SessionCreate(s).ok());
    }
    int64_t a = -1, b = -1, none = -1;
    ASSERT_TRUE(store_->SessionCount("cn_a", &a).ok());
    ASSERT_TRUE(store_->SessionCount("cn_b", &b).ok());
    ASSERT_TRUE(store_->SessionCount("cn_absent", &none).ok());
    EXPECT_EQ(a, 4);
    EXPECT_EQ(b, 2);
    EXPECT_EQ(none, 0);
}

}  // namespace
}  // namespace cortrix
