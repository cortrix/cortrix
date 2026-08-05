#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include <map>
#include <memory>
#include <queue>
#include <string>
#include <vector>
#include <chrono>
#include <ctime>

#include "cortrix/memory/contradiction_detector.h"
#include "cortrix/memory/memory_extract_error.h"
#include "cortrix/memory/memory_extract_metrics.h"
#include "cortrix/memory/memory_extractor.h"

// S3/S4/S5/S7 coverage: the memory extraction LLM extraction pipeline against injected seams +
// a scriptable LLM client. Includes the §12.3 eight LLM-mock scenarios (fact /
// preference / event classification, new-fact contradiction, non-contradiction,
// unknown→event fallback, invalid JSON, timeout), the D6 invalidation + operation_log
// writes, the D9 revoke path, and the D8 preference-immunity helper.
namespace cortrix::memory {
namespace {

// Build an ISO-8601 UTC timestamp `days` days before now. D8 immunity asserts a
// "within window" condition against the real wall clock (MemoryExtractor uses
// NowIso8601()), so a hard-coded absolute first_mentioned_at silently crosses the
// 30-day window boundary as the calendar advances (the 2026-06-14 S2b time-bomb:
// hard-coded 2026-05-15 + 30d == today). A relative timestamp keeps the test stable.
inline std::string IsoDaysAgo(int days) {
    const auto tp = std::chrono::system_clock::now() -
                    std::chrono::hours(static_cast<long long>(24) * days);
    const std::time_t t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm_utc{};
#if defined(_WIN32)
    gmtime_s(&tm_utc, &t);
#else
    gmtime_r(&t, &tm_utc);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
    return std::string(buf);
}

using ::testing::_;

// --- Scriptable LLM client: returns queued responses in order. The contradiction
// path makes a second LLM call (the judge), so the extraction tests queue the
// extraction response first, then any judge responses. ---
class ScriptedLlmClient : public llm::ILlmClient {
public:
    void PushOk(const std::string& content, int prompt_tokens = 100,
                int completion_tokens = 40, const std::string& model = "gpt-4o-mini") {
        llm::ChatCompletionResponse r;
        r.status = Status::Ok();
        r.content = content;
        r.model = model;
        r.finish_reason = "stop";
        r.prompt_tokens = prompt_tokens;
        r.completion_tokens = completion_tokens;
        responses_.push(r);
    }
    void PushError(StatusCode code, const std::string& msg) {
        llm::ChatCompletionResponse r;
        r.status = Status(code, msg);
        responses_.push(r);
    }
    llm::ChatCompletionResponse Chat(const std::string& prompt,
                                     const llm::LlmCallConfig& config) override {
        (void)prompt;
        (void)config;
        ++call_count;
        if (responses_.empty()) {
            llm::ChatCompletionResponse r;
            r.status = Status::Internal("no scripted response");
            return r;
        }
        auto r = responses_.front();
        responses_.pop();
        return r;
    }
    int call_count = 0;

private:
    std::queue<llm::ChatCompletionResponse> responses_;
};

// --- Fake memory-block store (in-memory map) ---
class FakeBlockStore : public IMemoryBlockStore {
public:
    Result<std::string> InsertMemoryBlock(const MemoryBlockRecord& block) override {
        blocks_[block.block_id] = block;
        inserted.push_back(block.block_id);
        return block.block_id;
    }
    Status UpdateMemoryBlock(const MemoryBlockRecord& block) override {
        if (!blocks_.count(block.block_id)) {
            return Status::NotFound("no such block: " + block.block_id);
        }
        blocks_[block.block_id] = block;
        return Status::Ok();
    }
    Result<MemoryBlockRecord> GetMemoryBlock(const std::string& block_id) override {
        auto it = blocks_.find(block_id);
        if (it == blocks_.end()) return Status::NotFound("no such block: " + block_id);
        return it->second;
    }
    void Seed(const MemoryBlockRecord& rec) { blocks_[rec.block_id] = rec; }
    const MemoryBlockRecord* Get(const std::string& id) const {
        auto it = blocks_.find(id);
        return it == blocks_.end() ? nullptr : &it->second;
    }
    std::vector<std::string> inserted;

private:
    std::map<std::string, MemoryBlockRecord> blocks_;
};

// --- Fake contradiction-query seam: returns preset candidates ---
class FakeContradictionQuery : public IContradictionQuery {
public:
    std::vector<CandidateBlock> FindCandidates(const std::string& ns,
                                               const std::string& content,
                                               int top_k) override {
        (void)ns;
        (void)content;
        (void)top_k;
        return candidates;
    }
    std::optional<CandidateBlock> FindMatchingPreference(const std::string& ns,
                                                         const std::string& content) override {
        (void)ns;
        (void)content;
        return preference_match;
    }
    std::vector<CandidateBlock> candidates;
    std::optional<CandidateBlock> preference_match;  // nullopt = first mention
};

// --- A no-op operation logger so the write path exercises op_logger_ branches ---
class CountingOpLogger : public observability::IOperationLogger {
public:
    void Log(const observability::OperationLogEntry& entry,
             const observability::TraceContext* ctx = nullptr) override {
        (void)ctx;
        logged.push_back(entry);
    }
    void BatchLog(const std::vector<observability::OperationLogEntry>& entries,
                  const observability::TraceContext* ctx = nullptr) override {
        (void)ctx;
        for (const auto& e : entries) logged.push_back(e);
    }
    Result<observability::OperationLogQueryResult> Query(
        const observability::OperationLogFilter&,
        const observability::TraceContext* = nullptr) override {
        return observability::OperationLogQueryResult{};
    }
    void Cleanup() override {}
    observability::OperationLogStats GetStats() override { return {}; }
    observability::HealthStatus Health() override { return {}; }

    int CountAction(const std::string& action) const {
        int n = 0;
        for (const auto& e : logged) if (e.action == action) ++n;
        return n;
    }
    const observability::OperationLogEntry* LastWithAction(const std::string& action) const {
        for (auto it = logged.rbegin(); it != logged.rend(); ++it)
            if (it->action == action) return &*it;
        return nullptr;
    }
    std::vector<observability::OperationLogEntry> logged;
};

InteractionLog MakeTurn(const std::string& id, const std::string& role,
                        const std::string& content) {
    InteractionLog i;
    i.id = id;
    i.session_id = "s_456";
    i.namespace_name = "memory";
    i.user_id = "user_123";
    i.role = role;
    i.content = content;
    return i;
}

// Like MakeTurn but with empty user_id AND empty namespace — exercises the
// `user_id.empty() ? "anonymous"` true arm and the `!ns.empty()` false arm in the
// operation_log writers (memory_extractor.cpp 401/405/504/508/521/527/615/619).
InteractionLog MakeTurnNoUserNoNs(const std::string& id, const std::string& role,
                                  const std::string& content) {
    InteractionLog i;
    i.id = id;
    i.session_id = "s_456";
    i.namespace_name = "";   // empty ns -> namespace_id not set
    i.user_id = "";          // empty user -> "anonymous" arm
    i.role = role;
    i.content = content;
    return i;
}

// Fixture wiring the four seams + the extractor.
class MemoryExtractorTest : public ::testing::Test {
protected:
    void SetUp() override {
        MemoryExtractMetrics::Instance().ResetForTest();
        llm_ = std::make_shared<ScriptedLlmClient>();
        store_ = std::make_shared<FakeBlockStore>();
        cq_ = std::make_shared<FakeContradictionQuery>();
        oplog_ = std::make_shared<CountingOpLogger>();
    }
    void TearDown() override { MemoryExtractMetrics::Instance().ResetForTest(); }

    std::unique_ptr<MemoryExtractor> Make(MemoryExtractorConfig cfg = {}) {
        return std::make_unique<MemoryExtractor>(llm_, store_, cq_, oplog_, cfg);
    }

    std::shared_ptr<ScriptedLlmClient> llm_;
    std::shared_ptr<FakeBlockStore> store_;
    std::shared_ptr<FakeContradictionQuery> cq_;
    std::shared_ptr<CountingOpLogger> oplog_;
};

// ---------- Pure helpers ----------

TEST_F(MemoryExtractorTest, ParseMemoryTypeKnownAndUnknown) {
    EXPECT_EQ(ParseMemoryType("fact"), MemoryType::kFact);
    EXPECT_EQ(ParseMemoryType("Preference"), MemoryType::kPreference);
    EXPECT_EQ(ParseMemoryType("EVENT"), MemoryType::kEvent);
    EXPECT_EQ(ParseMemoryType("nonsense"), MemoryType::kUnknown);
}

TEST_F(MemoryExtractorTest, BuildWindowTextTakesTailToConfiguredSize) {
    MemoryExtractorConfig cfg;
    cfg.window_size_default = 2;
    auto ex = Make(cfg);
    std::vector<InteractionLog> window = {
        MakeTurn("i1", "user", "first"),
        MakeTurn("i2", "assistant", "second"),
        MakeTurn("i3", "user", "third"),
    };
    std::string text = ex->BuildWindowText(window);
    EXPECT_EQ(text.find("first"), std::string::npos);   // dropped (tail of 2)
    EXPECT_NE(text.find("second"), std::string::npos);
    EXPECT_NE(text.find("third"), std::string::npos);
    EXPECT_NE(text.find("user: third"), std::string::npos);
}

// Confidence clamping (lines 144-145): values outside [0,1] are clamped.
TEST_F(MemoryExtractorTest, ConfidenceClampedToUnitRange) {
    llm_->PushOk(R"([
        {"type":"fact","content":"over one","confidence":1.7},
        {"type":"fact","content":"below zero","confidence":-0.4}
    ])");
    auto ex = Make();
    auto r = ex->ExtractFromWindow({MakeTurn("i1", "user", "x")});
    ASSERT_TRUE(r.ok());
    ASSERT_EQ(r.extracted_memories.size(), 2u);
    EXPECT_DOUBLE_EQ(r.extracted_memories[0].confidence, 1.0);  // clamped down
    EXPECT_DOUBLE_EQ(r.extracted_memories[1].confidence, 0.0);  // clamped up
}

// ---------- §12.3 LLM-mock scenarios ----------

// Scenario 1: fact classification.
TEST_F(MemoryExtractorTest, S1_FactClassification) {
    llm_->PushOk(R"([{"type":"fact","content":"user is a doctor","confidence":0.95}])");
    auto ex = Make();
    auto r = ex->ExtractFromWindow({MakeTurn("i1", "user", "I am a doctor")});
    ASSERT_TRUE(r.ok()) << (r.error ? r.error->message : "");
    ASSERT_EQ(r.extracted_memories.size(), 1u);
    EXPECT_EQ(r.extracted_memories[0].type, MemoryType::kFact);
    EXPECT_EQ(r.extracted_memories[0].content, "user is a doctor");
    EXPECT_FALSE(r.extracted_memories[0].block_id.empty());  // ULID minted
}

// R4/GLM: chat models routinely wrap the JSON array in a ```json … ``` markdown
// fence (and add prose) even with response_format=json_object. The parser must
// tolerate that — otherwise live GLM extraction fails as kExtractInvalidOutput.
TEST_F(MemoryExtractorTest, S1b_FencedAndProseWrappedJsonStillParses) {
    llm_->PushOk("Here are the extracted facts:\n```json\n"
                 R"([{"type":"fact","content":"user is a doctor","confidence":0.95}])"
                 "\n```\n");
    auto ex = Make();
    auto r = ex->ExtractFromWindow({MakeTurn("i1", "user", "I am a doctor")});
    ASSERT_TRUE(r.ok()) << (r.error ? r.error->message : "");
    ASSERT_EQ(r.extracted_memories.size(), 1u);
    EXPECT_EQ(r.extracted_memories[0].type, MemoryType::kFact);
    EXPECT_EQ(r.extracted_memories[0].content, "user is a doctor");
}

// Some providers in json_object mode nest the array in an object; narrowing to the
// outermost [ … ] span recovers it.
TEST_F(MemoryExtractorTest, S1c_ObjectWrappedArrayStillParses) {
    llm_->PushOk(R"({"memories":[{"type":"fact","content":"user is a doctor"}]})");
    auto ex = Make();
    auto r = ex->ExtractFromWindow({MakeTurn("i1", "user", "I am a doctor")});
    ASSERT_TRUE(r.ok()) << (r.error ? r.error->message : "");
    ASSERT_EQ(r.extracted_memories.size(), 1u);
    EXPECT_EQ(r.extracted_memories[0].content, "user is a doctor");
}

// Scenario 2: preference classification.
TEST_F(MemoryExtractorTest, S2_PreferenceClassification) {
    llm_->PushOk(R"([{"type":"preference","content":"user likes the dark theme","confidence":0.9}])");
    auto ex = Make();
    auto r = ex->ExtractFromWindow({MakeTurn("i1", "user", "I like the dark theme")});
    ASSERT_TRUE(r.ok());
    ASSERT_EQ(r.extracted_memories.size(), 1u);
    EXPECT_EQ(r.extracted_memories[0].type, MemoryType::kPreference);
    // Preference blocks carry mention_count + first_mentioned_at (D8).
    const MemoryBlockRecord* b = store_->Get(r.extracted_memories[0].block_id);
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(b->metadata_json["mention_count"], 1);
    EXPECT_TRUE(b->metadata_json.contains("first_mentioned_at"));
}

// D8 preference upgrade: a repeat mention increments the existing block's
// mention_count and marks it immune at the threshold (N=2), instead of inserting a
// duplicate. The existing-preference match comes from the (mocked) read-pipeline seam.
TEST_F(MemoryExtractorTest, S2b_RepeatPreferenceUpgradesMentionCountAndImmunity) {
    // Seed the first-mention preference block.
    MemoryBlockRecord pref;
    pref.block_id = "block_pref_dark";
    pref.ns_id = "memory";
    pref.user_id = "user_123";
    pref.content = "user likes the dark theme";
    // (Time-bomb fix) relative date so the D8 "within 30d window" immunity assertion
    // below stays valid regardless of the wall-clock date (was hard-coded 2026-05-15).
    pref.metadata_json = {{"memory_type", "preference"}, {"status", "active"},
                          {"mention_count", 1}, {"first_mentioned_at", IsoDaysAgo(1)}};
    store_->Seed(pref);
    cq_->preference_match =
        CandidateBlock{"block_pref_dark", "user likes the dark theme", MemoryType::kPreference, 0.95};

    // Re-extracting the same preference (second mention).
    llm_->PushOk(R"([{"type":"preference","content":"user likes the dark theme","confidence":0.9}])");
    auto ex = Make();  // default threshold N=2, 30d window
    auto r = ex->ExtractFromWindow({MakeTurn("i2", "user", "I still like the dark theme")});
    ASSERT_TRUE(r.ok());

    // No new block inserted — the existing one was upgraded.
    EXPECT_TRUE(store_->inserted.empty());
    const MemoryBlockRecord* b = store_->Get("block_pref_dark");
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(b->metadata_json["mention_count"], 2);
    EXPECT_EQ(b->metadata_json["immune"], true);  // 1+1 >= 2 within window
    EXPECT_EQ(b->metadata_json["status"], "active");
    EXPECT_EQ(oplog_->CountAction("memory_blocks_update"), 1);
}

TEST_F(MemoryExtractorTest, S2c_RepeatPreferenceBelowThresholdNotYetImmune) {
    MemoryBlockRecord pref;
    pref.block_id = "block_pref";
    pref.ns_id = "memory";
    pref.content = "user prefers Python";
    pref.metadata_json = {{"memory_type", "preference"}, {"status", "active"},
                          {"mention_count", 1}, {"first_mentioned_at", "2026-05-15T10:00:00Z"}};
    store_->Seed(pref);
    cq_->preference_match =
        CandidateBlock{"block_pref", "user prefers Python", MemoryType::kPreference, 0.95};

    llm_->PushOk(R"([{"type":"preference","content":"user prefers Python","confidence":0.9}])");
    MemoryExtractorConfig cfg;
    cfg.preference_immunization_threshold = 3;  // need 3 mentions
    auto ex = Make(cfg);
    auto r = ex->ExtractFromWindow({MakeTurn("i2", "user", "I use Python")});
    ASSERT_TRUE(r.ok());
    const MemoryBlockRecord* b = store_->Get("block_pref");
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(b->metadata_json["mention_count"], 2);   // incremented
    EXPECT_EQ(b->metadata_json["immune"], false);      // 2 < 3, not yet immune
}

// MaybeUpgradePreference: the preference seam reports a match, but the block is
// gone from the store (GetMemoryBlock fails, line 367) → upgrade returns false and
// the caller inserts a fresh block instead.
TEST_F(MemoryExtractorTest, S2d_PreferenceMatchButBlockMissingInsertsFresh) {
    // Match points at a block_id never seeded → GetMemoryBlock NotFound.
    cq_->preference_match =
        CandidateBlock{"vanished_pref", "user likes tea", MemoryType::kPreference, 0.9};
    llm_->PushOk(R"([{"type":"preference","content":"user likes tea","confidence":0.9}])");
    auto ex = Make();
    auto r = ex->ExtractFromWindow({MakeTurn("i1", "user", "I like tea")});
    ASSERT_TRUE(r.ok());
    // Fresh block inserted (upgrade path bailed out).
    ASSERT_EQ(store_->inserted.size(), 1u);
    const MemoryBlockRecord* b = store_->Get(r.extracted_memories[0].block_id);
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(b->metadata_json["mention_count"], 1);  // first-mention defaults
}

// MaybeUpgradePreference: the matched block has a non-object metadata_json and no
// mention_count / first_mentioned_at — the defensive resets (lines 369/375/380)
// default count to 1 and first_at to now, then increment to 2.
TEST_F(MemoryExtractorTest, S2e_PreferenceUpgradeOnNonObjectMetadataUsesDefaults) {
    MemoryBlockRecord pref;
    pref.block_id = "pref_blob";
    pref.ns_id = "memory";
    pref.content = "user likes coffee";
    pref.metadata_json = nlohmann::json("not-an-object");  // forces object reset
    store_->Seed(pref);
    cq_->preference_match =
        CandidateBlock{"pref_blob", "user likes coffee", MemoryType::kPreference, 0.9};

    llm_->PushOk(R"([{"type":"preference","content":"user likes coffee","confidence":0.9}])");
    auto ex = Make();
    auto r = ex->ExtractFromWindow({MakeTurn("i2", "user", "still coffee")});
    ASSERT_TRUE(r.ok());
    EXPECT_TRUE(store_->inserted.empty());  // upgraded, not inserted
    const MemoryBlockRecord* b = store_->Get("pref_blob");
    ASSERT_NE(b, nullptr);
    ASSERT_TRUE(b->metadata_json.is_object());
    EXPECT_EQ(b->metadata_json["mention_count"], 2);  // default 1 -> +1
}

// Scenario 3: event classification.
TEST_F(MemoryExtractorTest, S3_EventClassification) {
    llm_->PushOk(R"([{"type":"event","content":"user asked about the weather yesterday","confidence":0.85}])");
    auto ex = Make();
    auto r = ex->ExtractFromWindow({MakeTurn("i1", "user", "I asked about the weather yesterday")});
    ASSERT_TRUE(r.ok());
    ASSERT_EQ(r.extracted_memories.size(), 1u);
    EXPECT_EQ(r.extracted_memories[0].type, MemoryType::kEvent);
}

// Scenario 4: new fact contradicts old fact → invalidate old.
TEST_F(MemoryExtractorTest, S4_NewFactContradictsOldFact_InvalidatesOld) {
    // Seed an existing "user is in Shanghai" fact block.
    MemoryBlockRecord old_block;
    old_block.block_id = "block_old_sh";
    old_block.ns_id = "memory";
    old_block.user_id = "user_123";
    old_block.content = "user is in Shanghai";
    old_block.metadata_json = {{"memory_type", "fact"}, {"status", "active"}};
    store_->Seed(old_block);

    // The query seam returns that old block as a candidate.
    cq_->candidates = {CandidateBlock{"block_old_sh", "user is in Shanghai", MemoryType::kFact, 0.9}};

    // Extraction response (new fact) then judge response (contradiction, conf 0.75).
    llm_->PushOk(R"([{"type":"fact","content":"user is in Beijing","confidence":0.95}])");
    llm_->PushOk(R"({"is_contradiction":true,"reason":"location conflict","confidence":0.75})");

    auto ex = Make();
    auto r = ex->ExtractFromWindow({MakeTurn("i_123", "user", "I moved to Beijing")});
    ASSERT_TRUE(r.ok());
    ASSERT_EQ(r.extracted_memories.size(), 1u);
    EXPECT_EQ(r.extract_meta->contradictions_found, 1);
    // New block records what it invalidates.
    EXPECT_EQ(r.extracted_memories[0].invalidates,
              (std::vector<std::string>{"block_old_sh"}));

    // Old block now invalidated with the §4.2 fields + auto_revoke_eligible (conf<0.7? no→false).
    const MemoryBlockRecord* updated = store_->Get("block_old_sh");
    ASSERT_NE(updated, nullptr);
    EXPECT_EQ(updated->metadata_json["status"], "invalidated");
    EXPECT_EQ(updated->metadata_json["invalidation_triggered_by"], "llm_auto");
    EXPECT_EQ(updated->metadata_json["invalidation_confidence"], 0.75);
    EXPECT_EQ(updated->metadata_json["auto_revoke_eligible"], false);  // 0.75 >= 0.7
    EXPECT_EQ(updated->metadata_json["invalidated_by_block_id"],
              r.extracted_memories[0].block_id);

    // operation_log: one memory_invalidate + one memory_extract.
    EXPECT_EQ(oplog_->CountAction("memory_invalidate"), 1);
    EXPECT_EQ(oplog_->CountAction("memory_extract"), 1);
    EXPECT_EQ(MemoryExtractMetrics::Instance().InvalidationCount(
                  MemoryExtractMetrics::TriggeredBy::kLlmAuto), 1u);
}

// Scenario 4b: low-confidence contradiction marks auto_revoke_eligible=true.
TEST_F(MemoryExtractorTest, S4b_LowConfidenceContradictionMarksAutoRevoke) {
    MemoryBlockRecord old_block;
    old_block.block_id = "block_old";
    old_block.ns_id = "memory";
    old_block.content = "user is in Shanghai";
    old_block.metadata_json = {{"memory_type", "fact"}, {"status", "active"}};
    store_->Seed(old_block);
    cq_->candidates = {CandidateBlock{"block_old", "user is in Shanghai", MemoryType::kFact, 0.9}};

    llm_->PushOk(R"([{"type":"fact","content":"user is in Beijing","confidence":0.9}])");
    llm_->PushOk(R"({"is_contradiction":true,"reason":"x","confidence":0.5})");  // < 0.7

    auto ex = Make();
    auto r = ex->ExtractFromWindow({MakeTurn("i1", "user", "moving")});
    ASSERT_TRUE(r.ok());
    const MemoryBlockRecord* updated = store_->Get("block_old");
    ASSERT_NE(updated, nullptr);
    EXPECT_EQ(updated->metadata_json["auto_revoke_eligible"], true);
}

// Scenario 5: non-contradiction (complementary) → no invalidation.
TEST_F(MemoryExtractorTest, S5_NonContradiction_NoInvalidation) {
    MemoryBlockRecord old_block;
    old_block.block_id = "block_work_sh";
    old_block.ns_id = "memory";
    old_block.content = "user works in Shanghai";
    old_block.metadata_json = {{"memory_type", "fact"}, {"status", "active"}};
    store_->Seed(old_block);
    cq_->candidates = {CandidateBlock{"block_work_sh", "user works in Shanghai", MemoryType::kFact, 0.8}};

    llm_->PushOk(R"([{"type":"fact","content":"user is traveling in Beijing","confidence":0.9}])");
    llm_->PushOk(R"({"is_contradiction":false,"reason":"independent dimension","confidence":0.9})");

    auto ex = Make();
    auto r = ex->ExtractFromWindow({MakeTurn("i1", "user", "I'm traveling in Beijing")});
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.extract_meta->contradictions_found, 0);
    const MemoryBlockRecord* updated = store_->Get("block_work_sh");
    ASSERT_NE(updated, nullptr);
    EXPECT_EQ(updated->metadata_json["status"], "active");  // untouched
    EXPECT_EQ(oplog_->CountAction("memory_invalidate"), 0);
}

// Scenario 6: LLM returns unknown memory_type → coerced to event (D4).
TEST_F(MemoryExtractorTest, S6_UnknownTypeFallsBackToEvent) {
    llm_->PushOk(R"([{"type":"mystery","content":"something","confidence":0.7}])");
    auto ex = Make();
    auto r = ex->ExtractFromWindow({MakeTurn("i1", "user", "x")});
    ASSERT_TRUE(r.ok());
    ASSERT_EQ(r.extracted_memories.size(), 1u);
    EXPECT_EQ(r.extracted_memories[0].type, MemoryType::kEvent);
}

// Scenario 6b: missing type field also → event.
TEST_F(MemoryExtractorTest, S6b_MissingTypeFallsBackToEvent) {
    llm_->PushOk(R"([{"content":"no type here","confidence":0.6}])");
    auto ex = Make();
    auto r = ex->ExtractFromWindow({MakeTurn("i1", "user", "x")});
    ASSERT_TRUE(r.ok());
    ASSERT_EQ(r.extracted_memories.size(), 1u);
    EXPECT_EQ(r.extracted_memories[0].type, MemoryType::kEvent);
}

// Scenario 7: LLM returns invalid JSON → CX_ERR_MEMEXTRACT_INVALID_OUTPUT.
TEST_F(MemoryExtractorTest, S7_InvalidJsonReturnsInvalidOutput) {
    llm_->PushOk("this is not json at all");
    auto ex = Make();
    auto r = ex->ExtractFromWindow({MakeTurn("i1", "user", "x")});
    ASSERT_FALSE(r.ok());
    ASSERT_TRUE(r.error.has_value());
    EXPECT_EQ(r.error->code, "CX_ERR_MEMEXTRACT_INVALID_OUTPUT");
    EXPECT_EQ(MemoryExtractMetrics::Instance().ExtractCount(
                  MemoryExtractMetrics::ExtractStatus::kFailed), 1u);
}

// Scenario 8: LLM timeout → CX_ERR_MEMEXTRACT_LLM_TIMEOUT + retryable.
TEST_F(MemoryExtractorTest, S8_LlmTimeoutReturnsTimeoutRetryable) {
    llm_->PushError(StatusCode::kUnavailable, "circuit open / timeout");
    auto ex = Make();
    auto r = ex->ExtractFromWindow({MakeTurn("i_123", "user", "x")});
    ASSERT_FALSE(r.ok());
    ASSERT_TRUE(r.error.has_value());
    EXPECT_EQ(r.error->code, "CX_ERR_MEMEXTRACT_LLM_TIMEOUT");
    EXPECT_TRUE(r.error->retryable);
    ASSERT_TRUE(r.error->structured_data.has_value());
    EXPECT_EQ((*r.error->structured_data)["interaction_id"], "i_123");
}

// ---------- D11 LLM_DISABLED (NullEnricher) ----------

TEST_F(MemoryExtractorTest, DisabledModeReturnsLlmDisabled) {
    MemoryExtractorConfig cfg;
    cfg.enabled = false;
    auto ex = Make(cfg);
    auto r = ex->ExtractFromWindow({MakeTurn("i1", "user", "x")});
    ASSERT_FALSE(r.ok());
    ASSERT_TRUE(r.error.has_value());
    EXPECT_EQ(r.error->code, "CX_ERR_MEMEXTRACT_LLM_DISABLED");
    EXPECT_FALSE(r.error->retryable);
    EXPECT_EQ(llm_->call_count, 0);  // no LLM call in disabled mode
}

// ---------- extract_meta completeness (D11) ----------

TEST_F(MemoryExtractorTest, ExtractMetaPopulated) {
    llm_->PushOk(R"([{"type":"fact","content":"user is a doctor","confidence":0.95}])",
                 /*prompt*/ 300, /*completion*/ 150);
    MemoryExtractorConfig cfg;
    cfg.tokens_cost_per_1k_usd = 0.01;
    auto ex = Make(cfg);
    auto r = ex->ExtractFromWindow({MakeTurn("i1", "user", "I am a doctor")});
    ASSERT_TRUE(r.ok());
    ASSERT_TRUE(r.extract_meta.has_value());
    EXPECT_EQ(r.extract_meta->llm_model, "gpt-4o-mini");
    EXPECT_EQ(r.extract_meta->tokens_used, 450);
    EXPECT_DOUBLE_EQ(r.extract_meta->tokens_cost_usd, 0.45 * 0.01);
    EXPECT_FALSE(r.extract_meta->extracted_at.empty());
}

// ---------- Multiple memories from one interaction ----------

TEST_F(MemoryExtractorTest, MultipleMemoriesExtracted) {
    llm_->PushOk(R"([
        {"type":"fact","content":"user is in Shanghai","confidence":0.95},
        {"type":"event","content":"user starts a new job next month","confidence":0.9}
    ])");
    auto ex = Make();
    auto r = ex->ExtractFromWindow({MakeTurn("i1", "user", "I just moved to Shanghai, starting a new job next month")});
    ASSERT_TRUE(r.ok());
    ASSERT_EQ(r.extracted_memories.size(), 2u);
    EXPECT_EQ(r.extracted_memories[0].type, MemoryType::kFact);
    EXPECT_EQ(r.extracted_memories[1].type, MemoryType::kEvent);
    EXPECT_EQ(store_->inserted.size(), 2u);
}

// ---------- Empty window guard ----------

TEST_F(MemoryExtractorTest, EmptyWindowReturnsInvalidOutput) {
    auto ex = Make();
    auto r = ex->ExtractFromWindow({});
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error->code, "CX_ERR_MEMEXTRACT_INVALID_OUTPUT");
}

// ---------- Events do not trigger contradiction judging ----------

TEST_F(MemoryExtractorTest, EventMemoriesSkipContradictionJudge) {
    cq_->candidates = {CandidateBlock{"block_x", "anything", MemoryType::kFact, 0.9}};
    llm_->PushOk(R"([{"type":"event","content":"user had a meeting yesterday","confidence":0.8}])");
    // No judge response queued — if the extractor tried to judge it would fail.
    auto ex = Make();
    auto r = ex->ExtractFromWindow({MakeTurn("i1", "user", "had a meeting yesterday")});
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.extract_meta->contradictions_found, 0);
    EXPECT_EQ(llm_->call_count, 1);  // only the extraction call, no judge call
}

// ---------- Batch extraction ----------

TEST_F(MemoryExtractorTest, ExtractBatchReturnsResultPerTurn) {
    llm_->PushOk(R"([{"type":"fact","content":"a","confidence":0.9}])");
    llm_->PushOk(R"([{"type":"event","content":"b","confidence":0.8}])");
    auto ex = Make();
    auto batch = ex->ExtractBatch({MakeTurn("i1", "user", "x"), MakeTurn("i2", "user", "y")});
    ASSERT_EQ(batch.results.size(), 2u);
    EXPECT_TRUE(batch.results[0].ok());
    EXPECT_TRUE(batch.results[1].ok());
}

// ---------- D9 revoke ----------

TEST_F(MemoryExtractorTest, RevokeInvalidationRestoresActiveAndLogs) {
    MemoryBlockRecord b;
    b.block_id = "block_sh";
    b.ns_id = "memory";
    b.user_id = "user_123";
    b.content = "user is in Shanghai";
    b.metadata_json = {{"memory_type", "fact"}, {"status", "invalidated"},
                       {"invalidated_by_block_id", "block_bj"}};
    store_->Seed(b);

    auto ex = Make();
    auto r = ex->RevokeInvalidation("block_sh", "a business trip is not a move", "agent_self");
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value().metadata_json["status"], "active");
    EXPECT_EQ(r.value().metadata_json["revoked_by"], "agent_self");
    const MemoryBlockRecord* updated = store_->Get("block_sh");
    ASSERT_NE(updated, nullptr);
    EXPECT_EQ(updated->metadata_json["status"], "active");
    EXPECT_EQ(oplog_->CountAction("memory_revoke"), 1);
    EXPECT_EQ(MemoryExtractMetrics::Instance().InvalidationCount(
                  MemoryExtractMetrics::TriggeredBy::kAgentSelf), 1u);
}

TEST_F(MemoryExtractorTest, RevokeMissingBlockReturnsNotFound) {
    auto ex = Make();
    auto r = ex->RevokeInvalidation("nope", "x", "user_manual");
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.status().code(), StatusCode::kNotFound);
}

// ---------- D9 manual invalidate ----------

TEST_F(MemoryExtractorTest, InvalidateMemoryManualStampsAndLogs) {
    MemoryBlockRecord b;
    b.block_id = "block_x";
    b.ns_id = "memory";
    b.user_id = "user_123";
    b.content = "user hates notifications";
    b.metadata_json = {{"memory_type", "preference"}, {"status", "active"}};
    store_->Seed(b);

    auto ex = Make();
    Status s = ex->InvalidateMemory("block_x", "user explicitly retired this");
    ASSERT_TRUE(s.ok());
    const MemoryBlockRecord* updated = store_->Get("block_x");
    ASSERT_NE(updated, nullptr);
    EXPECT_EQ(updated->metadata_json["status"], "invalidated");
    EXPECT_EQ(updated->metadata_json["invalidation_triggered_by"], "manual");
    EXPECT_EQ(oplog_->CountAction("memory_blocks_update"), 1);
}

// ---------- D8 preference immunity helper ----------

TEST_F(MemoryExtractorTest, PreferenceImmunityThresholdAndWindow) {
    // existing count 1 + this mention = 2 == threshold, within window → immune.
    EXPECT_TRUE(MemoryExtractor::IsPreferenceImmune(
        1, "2026-05-15T10:00:00Z", "2026-05-20T10:00:00Z", 2, 30));
    // count not yet reached (0 + 1 = 1 < 2) → not immune.
    EXPECT_FALSE(MemoryExtractor::IsPreferenceImmune(
        0, "2026-05-15T10:00:00Z", "2026-05-20T10:00:00Z", 2, 30));
    // count met but first mention outside the 30-day window → not immune.
    EXPECT_FALSE(MemoryExtractor::IsPreferenceImmune(
        1, "2026-01-01T10:00:00Z", "2026-05-20T10:00:00Z", 2, 30));
    // count met, malformed timestamps → conservative immune-on-count.
    EXPECT_TRUE(MemoryExtractor::IsPreferenceImmune(1, "", "", 2, 30));
}

// ---------- Direct judgment-parse contract ----------

TEST_F(MemoryExtractorTest, ParseJudgmentJsonValidAndInvalid) {
    auto ex = Make();
    auto ok = ex->ParseJudgmentJson(R"({"is_contradiction":true,"reason":"r","confidence":0.8})");
    ASSERT_TRUE(ok.ok());
    EXPECT_TRUE(ok.value().is_contradiction);
    EXPECT_EQ(ok.value().reason, "r");
    EXPECT_DOUBLE_EQ(ok.value().confidence, 0.8);

    auto bad = ex->ParseJudgmentJson("not json");
    EXPECT_FALSE(bad.ok());

    auto missing = ex->ParseJudgmentJson(R"({"reason":"no verdict"})");
    EXPECT_FALSE(missing.ok());
}

// ---------- ContradictionDetector standalone ----------

TEST_F(MemoryExtractorTest, ContradictionDetectorMapsTimeout) {
    auto llm = std::make_shared<ScriptedLlmClient>();
    llm->PushError(StatusCode::kUnavailable, "timeout");
    ContradictionDetector det(llm, "gpt-4o-mini", 30000);
    auto r = det.Judge("user is in Beijing", "user is in Shanghai");
    ASSERT_FALSE(r.ok());
    EXPECT_NE(r.status().message().find("CX_ERR_MEMEXTRACT_LLM_TIMEOUT"),
              std::string::npos);
}

TEST_F(MemoryExtractorTest, ContradictionDetectorMapsGenericErrorToAmbiguous) {
    auto llm = std::make_shared<ScriptedLlmClient>();
    llm->PushError(StatusCode::kInternal, "model exploded");
    ContradictionDetector det(llm, "gpt-4o-mini", 30000);
    auto r = det.Judge("a", "b");
    ASSERT_FALSE(r.ok());
    EXPECT_NE(r.status().message().find("CX_ERR_MEMEXTRACT_CONTRADICTION_AMBIGUOUS"),
              std::string::npos);
}

TEST_F(MemoryExtractorTest, ContradictionDetectorNullLlmReturnsDisabled) {
    ContradictionDetector det(nullptr, "gpt-4o-mini", 30000);
    auto r = det.Judge("a", "b");
    ASSERT_FALSE(r.ok());
    EXPECT_NE(r.status().message().find("CX_ERR_MEMEXTRACT_LLM_DISABLED"), std::string::npos);
}

TEST_F(MemoryExtractorTest, ContradictionDetectorParsesNumericBoolean) {
    // is_contradiction given as 1 / 0 (some models emit numbers).
    auto t = ContradictionDetector::ParseJudgmentJson(
        R"({"is_contradiction":1,"reason":"r","confidence":0.7})");
    ASSERT_TRUE(t.ok());
    EXPECT_TRUE(t.value().is_contradiction);
    auto f = ContradictionDetector::ParseJudgmentJson(R"({"is_contradiction":0})");
    ASSERT_TRUE(f.ok());
    EXPECT_FALSE(f.value().is_contradiction);
}

TEST_F(MemoryExtractorTest, ContradictionDetectorRejectsNonBoolVerdict) {
    auto r = ContradictionDetector::ParseJudgmentJson(
        R"({"is_contradiction":"maybe"})");
    ASSERT_FALSE(r.ok());
    EXPECT_NE(r.status().message().find("CX_ERR_MEMEXTRACT_INVALID_OUTPUT"),
              std::string::npos);
}

TEST_F(MemoryExtractorTest, ContradictionDetectorSuccessPathReturnsVerdict) {
    auto llm = std::make_shared<ScriptedLlmClient>();
    llm->PushOk(R"({"is_contradiction":true,"reason":"loc conflict","confidence":0.9})");
    ContradictionDetector det(llm, "gpt-4o-mini", 30000);
    auto r = det.Judge("user is in Beijing", "user is in Shanghai");
    ASSERT_TRUE(r.ok());
    EXPECT_TRUE(r.value().is_contradiction);
    EXPECT_DOUBLE_EQ(r.value().confidence, 0.9);
}

// ---------- Extraction error-branch coverage ----------

TEST_F(MemoryExtractorTest, BudgetExceededMapsToBudgetError) {
    llm_->PushError(StatusCode::kPermissionDenied, "daily budget cap hit");
    auto ex = Make();
    auto r = ex->ExtractFromWindow({MakeTurn("i1", "user", "x")});
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error->code, "CX_ERR_MEMEXTRACT_BUDGET_EXCEEDED");
    EXPECT_FALSE(r.error->retryable);
}

TEST_F(MemoryExtractorTest, GenericLlmErrorMapsToInvalidOutput) {
    llm_->PushError(StatusCode::kInternal, "unexpected");
    auto ex = Make();
    auto r = ex->ExtractFromWindow({MakeTurn("i1", "user", "x")});
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error->code, "CX_ERR_MEMEXTRACT_INVALID_OUTPUT");
}

TEST_F(MemoryExtractorTest, ExtractionEntryMissingContentIsInvalidOutput) {
    // Array element without a string "content" field → INVALID_OUTPUT.
    llm_->PushOk(R"([{"type":"fact","confidence":0.9}])");
    auto ex = Make();
    auto r = ex->ExtractFromWindow({MakeTurn("i1", "user", "x")});
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error->code, "CX_ERR_MEMEXTRACT_INVALID_OUTPUT");
}

// A block store whose insert always fails — exercises the persist-failure path.
class FailingBlockStore : public IMemoryBlockStore {
public:
    Result<std::string> InsertMemoryBlock(const MemoryBlockRecord&) override {
        return Status::Internal("disk full");
    }
    Status UpdateMemoryBlock(const MemoryBlockRecord&) override {
        return Status::Internal("disk full");
    }
    Result<MemoryBlockRecord> GetMemoryBlock(const std::string&) override {
        return Status::NotFound("none");
    }
};

TEST_F(MemoryExtractorTest, PersistFailureSurfacesInvalidOutput) {
    auto failing = std::make_shared<FailingBlockStore>();
    auto ex = std::make_unique<MemoryExtractor>(llm_, failing, cq_, oplog_,
                                                MemoryExtractorConfig{});
    llm_->PushOk(R"([{"type":"fact","content":"user is a doctor","confidence":0.95}])");
    auto r = ex->ExtractFromWindow({MakeTurn("i1", "user", "I am a doctor")});
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error->code, "CX_ERR_MEMEXTRACT_INVALID_OUTPUT");
    EXPECT_NE(r.error->message.find("persist failed"), std::string::npos);
}

TEST_F(MemoryExtractorTest, ContradictionWithVanishedOldBlockIsSkipped) {
    // Candidate references a block_id that the store does not have → invalidation
    // stamping is skipped (race tolerance), new memory still persisted.
    cq_->candidates = {CandidateBlock{"ghost_block", "user is in Shanghai", MemoryType::kFact, 0.9}};
    llm_->PushOk(R"([{"type":"fact","content":"user is in Beijing","confidence":0.9}])");
    llm_->PushOk(R"({"is_contradiction":true,"reason":"x","confidence":0.8})");
    auto ex = Make();
    auto r = ex->ExtractFromWindow({MakeTurn("i1", "user", "moving")});
    ASSERT_TRUE(r.ok());
    // The contradiction was detected but the old block was gone → not stamped.
    EXPECT_EQ(r.extract_meta->contradictions_found, 1);
    EXPECT_EQ(oplog_->CountAction("memory_invalidate"), 0);
}

// Null-seam guards: an extractor with no block store fails the admin ops cleanly.
TEST_F(MemoryExtractorTest, RevokeWithNullStoreFails) {
    auto ex = std::make_unique<MemoryExtractor>(llm_, nullptr, cq_, oplog_,
                                                MemoryExtractorConfig{});
    auto r = ex->RevokeInvalidation("b", "x", "user_manual");
    EXPECT_FALSE(r.ok());
}

TEST_F(MemoryExtractorTest, InvalidateWithNullStoreFails) {
    auto ex = std::make_unique<MemoryExtractor>(llm_, nullptr, cq_, oplog_,
                                                MemoryExtractorConfig{});
    EXPECT_FALSE(ex->InvalidateMemory("b", "x").ok());
}

TEST_F(MemoryExtractorTest, InvalidateMissingBlockReturnsNotFound) {
    auto ex = Make();
    Status s = ex->InvalidateMemory("nope", "x");
    EXPECT_EQ(s.code(), StatusCode::kNotFound);
}

// Judge returns an error mid-extraction → that candidate is skipped (no
// invalidation on an unreliable verdict), extraction still succeeds (L337 continue).
TEST_F(MemoryExtractorTest, AmbiguousJudgeDuringExtractSkipsCandidate) {
    MemoryBlockRecord old_block;
    old_block.block_id = "block_old";
    old_block.ns_id = "memory";
    old_block.content = "user is in Shanghai";
    old_block.metadata_json = {{"memory_type", "fact"}, {"status", "active"}};
    store_->Seed(old_block);
    cq_->candidates = {CandidateBlock{"block_old", "user is in Shanghai", MemoryType::kFact, 0.9}};

    llm_->PushOk(R"([{"type":"fact","content":"user is in Beijing","confidence":0.9}])");
    llm_->PushError(StatusCode::kInternal, "judge unsure");  // judge fails → skip

    auto ex = Make();
    auto r = ex->ExtractFromWindow({MakeTurn("i1", "user", "moving")});
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.extract_meta->contradictions_found, 0);  // no confirmed contradiction
    const MemoryBlockRecord* updated = store_->Get("block_old");
    ASSERT_NE(updated, nullptr);
    EXPECT_EQ(updated->metadata_json["status"], "active");  // untouched
}

// Old block with a non-object metadata_json is reset to an object before stamping
// invalidation (defensive branch L485).
TEST_F(MemoryExtractorTest, InvalidationResetsNonObjectMetadata) {
    MemoryBlockRecord old_block;
    old_block.block_id = "block_old";
    old_block.ns_id = "memory";
    old_block.content = "user is in Shanghai";
    old_block.metadata_json = nlohmann::json("not-an-object");  // malformed
    store_->Seed(old_block);
    cq_->candidates = {CandidateBlock{"block_old", "user is in Shanghai", MemoryType::kFact, 0.9}};

    llm_->PushOk(R"([{"type":"fact","content":"user is in Beijing","confidence":0.9}])");
    llm_->PushOk(R"({"is_contradiction":true,"reason":"x","confidence":0.8})");

    auto ex = Make();
    auto r = ex->ExtractFromWindow({MakeTurn("i1", "user", "moving")});
    ASSERT_TRUE(r.ok());
    const MemoryBlockRecord* updated = store_->Get("block_old");
    ASSERT_NE(updated, nullptr);
    ASSERT_TRUE(updated->metadata_json.is_object());
    EXPECT_EQ(updated->metadata_json["status"], "invalidated");
}

// Revoke on a block whose metadata_json is non-object resets it (defensive L555).
TEST_F(MemoryExtractorTest, RevokeResetsNonObjectMetadata) {
    MemoryBlockRecord b;
    b.block_id = "block_x";
    b.ns_id = "memory";
    b.metadata_json = nlohmann::json("not-an-object");
    store_->Seed(b);
    auto ex = Make();
    auto r = ex->RevokeInvalidation("block_x", "x", "user_manual");
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value().metadata_json["status"], "active");
}

// Manual invalidate on a block whose metadata_json is non-object resets it (L600).
TEST_F(MemoryExtractorTest, ManualInvalidateResetsNonObjectMetadata) {
    MemoryBlockRecord b;
    b.block_id = "block_x";
    b.ns_id = "memory";
    b.metadata_json = nlohmann::json(42);  // malformed
    store_->Seed(b);
    auto ex = Make();
    ASSERT_TRUE(ex->InvalidateMemory("block_x", "retire").ok());
    const MemoryBlockRecord* updated = store_->Get("block_x");
    ASSERT_NE(updated, nullptr);
    EXPECT_EQ(updated->metadata_json["status"], "invalidated");
}

// A store that inserts/reads fine but fails UPDATE — exercises the update-failure
// returns in the invalidation / revoke / manual-invalidate paths (L497/565/609).
class UpdateFailingStore : public IMemoryBlockStore {
public:
    Result<std::string> InsertMemoryBlock(const MemoryBlockRecord& b) override {
        blocks_[b.block_id] = b;
        return b.block_id;
    }
    Status UpdateMemoryBlock(const MemoryBlockRecord&) override {
        return Status::Internal("update rejected");
    }
    Result<MemoryBlockRecord> GetMemoryBlock(const std::string& id) override {
        auto it = blocks_.find(id);
        if (it == blocks_.end()) return Status::NotFound("none");
        return it->second;
    }
    void Seed(const MemoryBlockRecord& r) { blocks_[r.block_id] = r; }
    std::map<std::string, MemoryBlockRecord> blocks_;
};

TEST_F(MemoryExtractorTest, RevokeUpdateFailureReturnsError) {
    auto store = std::make_shared<UpdateFailingStore>();
    MemoryBlockRecord b;
    b.block_id = "block_x";
    b.metadata_json = {{"status", "invalidated"}};
    store->Seed(b);
    auto ex = std::make_unique<MemoryExtractor>(llm_, store, cq_, oplog_,
                                                MemoryExtractorConfig{});
    auto r = ex->RevokeInvalidation("block_x", "x", "user_manual");
    EXPECT_FALSE(r.ok());
}

TEST_F(MemoryExtractorTest, ManualInvalidateUpdateFailureReturnsError) {
    auto store = std::make_shared<UpdateFailingStore>();
    MemoryBlockRecord b;
    b.block_id = "block_x";
    b.metadata_json = {{"status", "active"}};
    store->Seed(b);
    auto ex = std::make_unique<MemoryExtractor>(llm_, store, cq_, oplog_,
                                                MemoryExtractorConfig{});
    EXPECT_FALSE(ex->InvalidateMemory("block_x", "x").ok());
}

// MaybeUpgradePreference: the matched block exists but UpdateMemoryBlock fails
// (line 397) → upgrade returns false, caller inserts a fresh block instead.
// (Defined here so UpdateFailingStore is already in scope.)
TEST_F(MemoryExtractorTest, S2f_PreferenceUpgradeUpdateFailureInsertsFresh) {
    auto store = std::make_shared<UpdateFailingStore>();
    MemoryBlockRecord pref;
    pref.block_id = "pref_x";
    pref.ns_id = "memory";
    pref.content = "user likes jazz";
    pref.metadata_json = {{"memory_type", "preference"}, {"status", "active"},
                          {"mention_count", 1}, {"first_mentioned_at", "2026-05-15T10:00:00Z"}};
    store->Seed(pref);
    cq_->preference_match =
        CandidateBlock{"pref_x", "user likes jazz", MemoryType::kPreference, 0.9};

    auto ex = std::make_unique<MemoryExtractor>(llm_, store, cq_, oplog_,
                                                MemoryExtractorConfig{});
    llm_->PushOk(R"([{"type":"preference","content":"user likes jazz","confidence":0.9}])");
    auto r = ex->ExtractFromWindow({MakeTurn("i2", "user", "jazz again")});
    ASSERT_TRUE(r.ok());
    // Upgrade failed on UPDATE -> a fresh block was inserted under a new ULID.
    EXPECT_FALSE(r.extracted_memories.empty());
    EXPECT_NE(r.extracted_memories[0].block_id, "pref_x");
}

// FindContradictions / MaybeUpgradePreference with a null contradiction-query seam
// degrade gracefully (no crash; treat as no-candidate / first-mention).
TEST_F(MemoryExtractorTest, NullContradictionQueryDegradesGracefully) {
    auto ex = std::make_unique<MemoryExtractor>(llm_, store_, nullptr, oplog_,
                                                MemoryExtractorConfig{});
    llm_->PushOk(R"([{"type":"fact","content":"user is in Beijing","confidence":0.9}])");
    auto r = ex->ExtractFromWindow({MakeTurn("i1", "user", "x")});
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.extract_meta->contradictions_found, 0);
}

// ToString totality for the sentinel enum values.
TEST_F(MemoryExtractorTest, ToStringCoversAllEnumValues) {
    EXPECT_STREQ(ToString(MemoryType::kFact), "fact");
    EXPECT_STREQ(ToString(MemoryType::kPreference), "preference");
    EXPECT_STREQ(ToString(MemoryType::kEvent), "event");
    EXPECT_STREQ(ToString(MemoryType::kUnknown), "unknown");
    EXPECT_STREQ(ToString(MemoryStatus::kActive), "active");
    EXPECT_STREQ(ToString(MemoryStatus::kTentative), "tentative");
    EXPECT_STREQ(ToString(MemoryStatus::kInvalidated), "invalidated");
}

// Extract() single-turn entry wraps the window correctly.
TEST_F(MemoryExtractorTest, ExtractSingleTurnEntry) {
    llm_->PushOk(R"([{"type":"fact","content":"user is a doctor","confidence":0.95}])");
    auto ex = Make();
    auto r = ex->Extract(MakeTurn("i1", "user", "I am a doctor"));
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.extracted_memories.size(), 1u);
}

// ---------- Untested conditional arms in the operation_log writers ----------
// Existing tests always wired a logger + non-empty user_id/ns, leaving the false /
// "anonymous" / empty-ns arms uncovered. These target them explicitly.

// Extract with NO op_logger: WriteWithOperationLog's `if (op_logger_)` false arms
// (lines 502/519) plus the invalidate-path false arm run; persistence still works.
TEST_F(MemoryExtractorTest, ExtractWithoutLoggerSkipsAuditButPersists) {
    auto ex = std::make_unique<MemoryExtractor>(llm_, store_, cq_, /*op_logger=*/nullptr,
                                                MemoryExtractorConfig{});
    llm_->PushOk(R"([{"type":"fact","content":"user is a doctor","confidence":0.95}])");
    auto r = ex->ExtractFromWindow({MakeTurn("i1", "user", "I am a doctor")});
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(store_->inserted.size(), 1u);  // block persisted without any audit
}

// Extract over a turn with empty user_id AND empty namespace: the memory_extract
// audit takes the `user_id.empty() ? "anonymous"` arm and the `!ns.empty()` false
// arm (lines 521/527). A logger IS wired so the audit branch runs.
TEST_F(MemoryExtractorTest, ExtractEmptyUserAndNsAnonymousAuditNoNamespace) {
    llm_->PushOk(R"([{"type":"fact","content":"a fact","confidence":0.9}])");
    auto ex = Make();
    auto r = ex->ExtractFromWindow({MakeTurnNoUserNoNs("i1", "user", "x")});
    ASSERT_TRUE(r.ok());
    const auto* e = oplog_->logged.empty() ? nullptr : &oplog_->logged.back();
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->action, "memory_extract");
    EXPECT_EQ(e->user_id, "anonymous");        // empty user_id -> "anonymous"
    EXPECT_FALSE(e->namespace_id.has_value());  // empty ns -> namespace_id unset
}

// Invalidation cascade with empty user_id + empty ns: the memory_invalidate audit
// inside WriteWithOperationLog takes the anonymous / no-namespace arms (504/508).
TEST_F(MemoryExtractorTest, InvalidationCascadeEmptyUserAnonymousAudit) {
    MemoryBlockRecord old_block;
    old_block.block_id = "block_old";
    old_block.ns_id = "";  // empty
    old_block.content = "user is in Shanghai";
    old_block.metadata_json = {{"memory_type", "fact"}, {"status", "active"}};
    store_->Seed(old_block);
    cq_->candidates = {CandidateBlock{"block_old", "user is in Shanghai", MemoryType::kFact, 0.9}};

    llm_->PushOk(R"([{"type":"fact","content":"user is in Beijing","confidence":0.9}])");
    llm_->PushOk(R"({"is_contradiction":true,"reason":"x","confidence":0.8})");
    auto ex = Make();
    auto r = ex->ExtractFromWindow({MakeTurnNoUserNoNs("i1", "user", "moving")});
    ASSERT_TRUE(r.ok());
    const auto* inv = oplog_->LastWithAction("memory_invalidate");
    ASSERT_NE(inv, nullptr);
    EXPECT_EQ(inv->user_id, "anonymous");
    EXPECT_FALSE(inv->namespace_id.has_value());
}

// RevokeInvalidation with empty revoked_by + empty ns_id: the memory_revoke audit
// takes the `revoked_by.empty() ? "anonymous"` arm and `!ns_id.empty()` false arm
// (lines 573/577), and the metric maps empty revoked_by to kManual (568 else arm).
TEST_F(MemoryExtractorTest, RevokeEmptyRevokerAnonymousAuditManualMetric) {
    MemoryBlockRecord b;
    b.block_id = "block_x";
    b.ns_id = "";  // empty
    b.metadata_json = {{"memory_type", "fact"}, {"status", "invalidated"}};
    store_->Seed(b);
    auto ex = Make();
    auto r = ex->RevokeInvalidation("block_x", "reason", /*revoked_by=*/"");
    ASSERT_TRUE(r.ok());
    const auto* e = oplog_->LastWithAction("memory_revoke");
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->user_id, "anonymous");
    EXPECT_FALSE(e->namespace_id.has_value());
    EXPECT_EQ(MemoryExtractMetrics::Instance().InvalidationCount(
                  MemoryExtractMetrics::TriggeredBy::kManual), 1u);
}

// RevokeInvalidation with NO op_logger: the `if (op_logger_)` false arm (571).
TEST_F(MemoryExtractorTest, RevokeWithoutLoggerStillSucceeds) {
    auto store = std::make_shared<FakeBlockStore>();
    MemoryBlockRecord b;
    b.block_id = "block_x";
    b.metadata_json = {{"memory_type", "fact"}, {"status", "invalidated"}};
    store->Seed(b);
    auto ex = std::make_unique<MemoryExtractor>(llm_, store, cq_, /*op_logger=*/nullptr,
                                                MemoryExtractorConfig{});
    auto r = ex->RevokeInvalidation("block_x", "reason", "agent_self");
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value().metadata_json["status"], "active");
}

// InvalidateMemory with empty block user_id + empty ns_id: the anonymous / no-ns
// arms (615/619); and with NO logger, the false arm (613).
TEST_F(MemoryExtractorTest, ManualInvalidateEmptyUserAnonymousAudit) {
    MemoryBlockRecord b;
    b.block_id = "block_x";
    b.ns_id = "";          // empty ns
    b.user_id = "";        // empty user -> "anonymous"
    b.metadata_json = {{"memory_type", "fact"}, {"status", "active"}};
    store_->Seed(b);
    auto ex = Make();
    ASSERT_TRUE(ex->InvalidateMemory("block_x", "retire").ok());
    const auto* e = oplog_->LastWithAction("memory_blocks_update");
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->user_id, "anonymous");
    EXPECT_FALSE(e->namespace_id.has_value());
}

TEST_F(MemoryExtractorTest, ManualInvalidateWithoutLoggerStillSucceeds) {
    auto store = std::make_shared<FakeBlockStore>();
    MemoryBlockRecord b;
    b.block_id = "block_x";
    b.metadata_json = {{"memory_type", "fact"}, {"status", "active"}};
    store->Seed(b);
    auto ex = std::make_unique<MemoryExtractor>(llm_, store, cq_, /*op_logger=*/nullptr,
                                                MemoryExtractorConfig{});
    ASSERT_TRUE(ex->InvalidateMemory("block_x", "x").ok());
    const MemoryBlockRecord* updated = store->Get("block_x");
    ASSERT_NE(updated, nullptr);
    EXPECT_EQ(updated->metadata_json["status"], "invalidated");
}

// Preference upgrade audit with NO logger: MaybeUpgradePreference's `if (op_logger_)`
// false arm (399). Upgrade still happens (mention_count increments).
TEST_F(MemoryExtractorTest, PreferenceUpgradeWithoutLoggerStillUpgrades) {
    auto store = std::make_shared<FakeBlockStore>();
    MemoryBlockRecord pref;
    pref.block_id = "pref_x";
    pref.content = "user likes tea";
    pref.metadata_json = {{"memory_type", "preference"}, {"status", "active"},
                          {"mention_count", 1}, {"first_mentioned_at", "2026-05-15T10:00:00Z"}};
    store->Seed(pref);
    cq_->preference_match =
        CandidateBlock{"pref_x", "user likes tea", MemoryType::kPreference, 0.9};
    llm_->PushOk(R"([{"type":"preference","content":"user likes tea","confidence":0.9}])");
    auto ex = std::make_unique<MemoryExtractor>(llm_, store, cq_, /*op_logger=*/nullptr,
                                                MemoryExtractorConfig{});
    auto r = ex->ExtractFromWindow({MakeTurn("i2", "user", "tea again")});
    ASSERT_TRUE(r.ok());
    const MemoryBlockRecord* b = store->Get("pref_x");
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(b->metadata_json["mention_count"], 2);
}

// NOTE: the match-by-content loop's no-match arm (lines 470-471, where new_block_id
// stays empty) is physically unreachable: FindContradictions builds every pair FROM
// the extracted memories, so c.new_content always equals some new_memories[i].content
// and the loop always breaks on a match. Recorded as unreachable, not forced.

}  // namespace
}  // namespace cortrix::memory
