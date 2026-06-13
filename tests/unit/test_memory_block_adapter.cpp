#include "cortrix/memory/memory_block_adapter.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "cortrix/store/cortrix_store_sqlite.h"

// M1 — the real MemoryBlockAdapter + MemoryContradictionAdapter + QueryUserFacts over
// an in-memory CortrixStoreSqlite (owns-db path creates the blocks table + the F35/F40
// columns via the providers). No embedder/index → rows are stored without vectors,
// which is exactly the adapter's degrade path; the SQL metadata queries are what these
// tests exercise.
namespace cortrix::memory {
namespace {

nlohmann::json FactMeta(const std::string& user, const std::string& type,
                        const std::string& status, const std::string& created,
                        double conf) {
    return nlohmann::json{
        {"memory_type", type}, {"status", status}, {"user_id", user},
        {"created_at", created}, {"confidence", conf}};
}

class MemoryBlockAdapterTest : public ::testing::Test {
protected:
    void SetUp() override {
        store_ = std::make_unique<CortrixStoreSqlite>(":memory:");
        ASSERT_EQ(store_->Open(), 0);
        adapter_ = std::make_unique<MemoryBlockAdapter>(*store_, nullptr, nullptr);
    }

    MemoryBlockRecord MakeRec(const std::string& id, const std::string& user,
                              const std::string& content, const nlohmann::json& meta) {
        MemoryBlockRecord r;
        r.block_id = id;
        r.user_id = user;
        r.content = content;
        r.metadata_json = meta;
        return r;
    }

    std::unique_ptr<CortrixStoreSqlite> store_;
    std::unique_ptr<MemoryBlockAdapter> adapter_;
};

TEST_F(MemoryBlockAdapterTest, InsertAndGetRoundTrip) {
    auto rec = MakeRec("01MEMAAAA", "alice", "user is a doctor",
                       FactMeta("alice", "fact", "active", "2026-06-12T10:00:00Z", 0.95));
    auto ins = adapter_->InsertMemoryBlock(rec);
    ASSERT_TRUE(ins.ok());
    EXPECT_EQ(ins.value(), "01MEMAAAA");

    auto got = adapter_->GetMemoryBlock("01MEMAAAA");
    ASSERT_TRUE(got.ok());
    EXPECT_EQ(got.value().content, "user is a doctor");
    EXPECT_EQ(got.value().user_id, "alice");
    EXPECT_EQ(got.value().metadata_json.value("memory_type", ""), "fact");
}

TEST_F(MemoryBlockAdapterTest, ListByUserFiltersByOwnerAndOrdersNewestFirst) {
    ASSERT_TRUE(adapter_->InsertMemoryBlock(MakeRec(
        "01MEMA1", "alice", "in Shanghai",
        FactMeta("alice", "fact", "active", "2026-06-12T09:00:00Z", 0.9))).ok());
    ASSERT_TRUE(adapter_->InsertMemoryBlock(MakeRec(
        "01MEMA2", "alice", "likes python",
        FactMeta("alice", "preference", "active", "2026-06-12T11:00:00Z", 0.8))).ok());
    ASSERT_TRUE(adapter_->InsertMemoryBlock(MakeRec(
        "01MEMB1", "bob", "in Beijing",
        FactMeta("bob", "fact", "active", "2026-06-12T10:00:00Z", 0.9))).ok());

    auto alice = adapter_->ListByUser("alice");
    ASSERT_TRUE(alice.ok());
    ASSERT_EQ(alice.value().size(), 2u);
    // newest first
    EXPECT_EQ(alice.value()[0].content, "likes python");
    EXPECT_EQ(alice.value()[1].content, "in Shanghai");

    auto bob = adapter_->ListByUser("bob");
    ASSERT_TRUE(bob.ok());
    ASSERT_EQ(bob.value().size(), 1u);
    EXPECT_EQ(bob.value()[0].content, "in Beijing");
}

TEST_F(MemoryBlockAdapterTest, UpdateStampsInvalidation) {
    ASSERT_TRUE(adapter_->InsertMemoryBlock(MakeRec(
        "01MEMUPD", "alice", "in Shanghai",
        FactMeta("alice", "fact", "active", "2026-06-12T09:00:00Z", 0.9))).ok());

    auto rec = adapter_->GetMemoryBlock("01MEMUPD");
    ASSERT_TRUE(rec.ok());
    auto updated = rec.value();
    updated.metadata_json["status"] = "invalidated";
    updated.metadata_json["invalidated_at"] = "2026-06-12T12:00:00Z";
    ASSERT_TRUE(adapter_->UpdateMemoryBlock(updated).ok());

    auto after = adapter_->GetMemoryBlock("01MEMUPD");
    ASSERT_TRUE(after.ok());
    EXPECT_EQ(after.value().metadata_json.value("status", ""), "invalidated");
    // Active-only list no longer returns it.
    auto active = adapter_->ListByUser("alice");
    ASSERT_TRUE(active.ok());
    EXPECT_EQ(active.value().size(), 1u);  // ListByUser returns all owned (incl invalidated)
}

TEST_F(MemoryBlockAdapterTest, ContradictionCandidatesReturnsActiveFactsAndPreferences) {
    ASSERT_TRUE(adapter_->InsertMemoryBlock(MakeRec(
        "01MEMC1", "alice", "in Shanghai",
        FactMeta("alice", "fact", "active", "2026-06-12T09:00:00Z", 0.9))).ok());
    ASSERT_TRUE(adapter_->InsertMemoryBlock(MakeRec(
        "01MEMC2", "alice", "old event",
        FactMeta("alice", "event", "active", "2026-06-12T08:00:00Z", 0.5))).ok());

    MemoryContradictionAdapter contra(*store_);
    auto cands = contra.FindCandidates("alice", "in Beijing", 5);
    // event excluded; only the fact is a candidate.
    ASSERT_EQ(cands.size(), 1u);
    EXPECT_EQ(cands[0].content, "in Shanghai");
    EXPECT_EQ(cands[0].type, MemoryType::kFact);
}

TEST_F(MemoryBlockAdapterTest, FindMatchingPreferenceMatchesExactActiveText) {
    ASSERT_TRUE(adapter_->InsertMemoryBlock(MakeRec(
        "01MEMP1", "alice", "likes dark mode",
        FactMeta("alice", "preference", "active", "2026-06-12T09:00:00Z", 0.8))).ok());

    MemoryContradictionAdapter contra(*store_);
    auto match = contra.FindMatchingPreference("alice", "likes dark mode");
    ASSERT_TRUE(match.has_value());
    EXPECT_EQ(match->content, "likes dark mode");

    EXPECT_FALSE(contra.FindMatchingPreference("alice", "likes light mode").has_value());
}

TEST_F(MemoryBlockAdapterTest, QueryUserFactsReturnsActiveOnlyNewestFirst) {
    ASSERT_TRUE(adapter_->InsertMemoryBlock(MakeRec(
        "01MEMF1", "alice", "in Shanghai",
        FactMeta("alice", "fact", "active", "2026-06-12T09:00:00Z", 0.9))).ok());
    ASSERT_TRUE(adapter_->InsertMemoryBlock(MakeRec(
        "01MEMF2", "alice", "stale fact",
        FactMeta("alice", "fact", "invalidated", "2026-06-12T11:00:00Z", 0.9))).ok());
    ASSERT_TRUE(adapter_->InsertMemoryBlock(MakeRec(
        "01MEMF3", "alice", "likes python",
        FactMeta("alice", "preference", "active", "2026-06-12T10:00:00Z", 0.8))).ok());

    auto facts = QueryUserFacts(*store_, "alice", 20);
    ASSERT_TRUE(facts.ok());
    ASSERT_EQ(facts.value().size(), 2u);  // invalidated excluded
    EXPECT_EQ(facts.value()[0].content, "likes python");  // newest active
    EXPECT_EQ(facts.value()[0].memory_type, "preference");
    EXPECT_EQ(facts.value()[1].content, "in Shanghai");
    EXPECT_DOUBLE_EQ(facts.value()[1].confidence, 0.9);
}

}  // namespace
}  // namespace cortrix::memory
