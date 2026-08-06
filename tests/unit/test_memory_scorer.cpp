// Memory decay unit tests — MemoryScorer (classified decay scoring).
// Covers design unit-test matrix (~24 cases) plus extra boundary coverage
// to meet line >= 95% / branch >= 80% targets. Pure-compute, no I/O / mocks.
#include <gtest/gtest.h>

#include <cmath>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include "cortrix/memory_scorer.h"

namespace cortrix {
namespace {

// One day in seconds — used to synthesize created_at relative to a fixed "now".
constexpr int64_t kDay = 86400;
// Fixed reference "now" (2023-01-01T00:00:00Z) so tests are deterministic.
constexpr int64_t kNow = 1672531200;

MemoryDecayConfig DefaultConfig() {
    MemoryDecayConfig c;  // lambda=0.01, min_score=0.0, llm_available=false
    return c;
}

// created_at for a memory aged `age_days` relative to kNow.
int64_t CreatedAtAgedDays(int64_t age_days) { return kNow - age_days * kDay; }

MemoryCandidate MakeCandidate(uint64_t id, const std::string& type,
                              const std::string& status, double raw,
                              int64_t age_days) {
    MemoryCandidate c;
    c.block_id = id;
    c.content = "content-" + std::to_string(id);
    c.memory_type = type;
    c.status = status;
    c.raw_score = raw;
    c.created_at = CreatedAtAgedDays(age_days);
    return c;
}

// ---------------------------------------------------------------------------
// Score(): per-type decay
// ---------------------------------------------------------------------------

TEST(MemoryScorerScore, Fact_NoDecay) {
    MemoryScorer s(DefaultConfig());
    auto m = s.Score(1, "c", "fact", "active", 0.8, CreatedAtAgedDays(90), kNow);
    EXPECT_DOUBLE_EQ(m.decay_factor, 1.0);
    EXPECT_DOUBLE_EQ(m.final_score, 0.8);
    EXPECT_EQ(m.age_days, 90);
    EXPECT_EQ(m.block_id, 1u);
    EXPECT_EQ(m.memory_type, "fact");
}

TEST(MemoryScorerScore, Preference_NoDecay) {
    MemoryScorer s(DefaultConfig());
    auto m = s.Score(2, "c", "preference", "active", 0.7, CreatedAtAgedDays(180), kNow);
    EXPECT_DOUBLE_EQ(m.decay_factor, 1.0);
    EXPECT_DOUBLE_EQ(m.final_score, 0.7);
    EXPECT_EQ(m.age_days, 180);
}

TEST(MemoryScorerScore, Event_Decay_1Day) {
    MemoryScorer s(DefaultConfig());
    auto m = s.Score(3, "c", "event", "active", 0.8, CreatedAtAgedDays(1), kNow);
    EXPECT_NEAR(m.decay_factor, 0.990, 1e-3);
    EXPECT_NEAR(m.final_score, 0.792, 1e-3);
}

TEST(MemoryScorerScore, Event_Decay_30Days) {
    MemoryScorer s(DefaultConfig());
    auto m = s.Score(4, "c", "event", "active", 0.8, CreatedAtAgedDays(30), kNow);
    EXPECT_NEAR(m.decay_factor, 0.741, 1e-3);
    EXPECT_NEAR(m.final_score, 0.593, 1e-3);
}

TEST(MemoryScorerScore, Event_Decay_90Days) {
    MemoryScorer s(DefaultConfig());
    auto m = s.Score(5, "c", "event", "active", 0.8, CreatedAtAgedDays(90), kNow);
    EXPECT_NEAR(m.decay_factor, 0.407, 1e-3);
    EXPECT_NEAR(m.final_score, 0.325, 1e-3);
}

TEST(MemoryScorerScore, Event_Decay_180Days) {
    MemoryScorer s(DefaultConfig());
    auto m = s.Score(6, "c", "event", "active", 0.8, CreatedAtAgedDays(180), kNow);
    EXPECT_NEAR(m.decay_factor, 0.165, 1e-3);
    EXPECT_NEAR(m.final_score, 0.133, 1e-3);
}

TEST(MemoryScorerScore, Event_Decay_365Days) {
    MemoryScorer s(DefaultConfig());
    auto m = s.Score(7, "c", "event", "active", 0.8, CreatedAtAgedDays(365), kNow);
    EXPECT_NEAR(m.decay_factor, 0.026, 1e-3);
    EXPECT_NEAR(m.final_score, 0.021, 1e-3);
}

TEST(MemoryScorerScore, EmptyType_AsEvent) {
    MemoryScorer s(DefaultConfig());
    auto empty_m = s.Score(8, "c", "", "active", 0.8, CreatedAtAgedDays(30), kNow);
    auto event_m = s.Score(9, "c", "event", "active", 0.8, CreatedAtAgedDays(30), kNow);
    EXPECT_DOUBLE_EQ(empty_m.decay_factor, event_m.decay_factor);
    EXPECT_DOUBLE_EQ(empty_m.final_score, event_m.final_score);
}

TEST(MemoryScorerScore, UnknownType_AsEvent) {
    MemoryScorer s(DefaultConfig());
    auto unk_m = s.Score(10, "c", "opinion", "active", 0.8, CreatedAtAgedDays(30), kNow);
    auto event_m = s.Score(11, "c", "event", "active", 0.8, CreatedAtAgedDays(30), kNow);
    EXPECT_DOUBLE_EQ(unk_m.decay_factor, event_m.decay_factor);
}

TEST(MemoryScorerScore, ZeroAge_Event) {
    MemoryScorer s(DefaultConfig());
    auto m = s.Score(12, "c", "event", "active", 0.8, kNow, kNow);
    EXPECT_DOUBLE_EQ(m.decay_factor, 1.0);
    EXPECT_EQ(m.age_days, 0);
}

TEST(MemoryScorerScore, NegativeAge_ClampedToZero) {
    MemoryScorer s(DefaultConfig());
    // created_at in the future -> age clamps to 0, no decay.
    auto m = s.Score(13, "c", "event", "active", 0.8, kNow + 10 * kDay, kNow);
    EXPECT_EQ(m.age_days, 0);
    EXPECT_DOUBLE_EQ(m.decay_factor, 1.0);
    EXPECT_DOUBLE_EQ(m.final_score, 0.8);
}

TEST(MemoryScorerScore, CreatedAtZero_NoDecay) {
    MemoryScorer s(DefaultConfig());
    auto m = s.Score(14, "c", "event", "active", 0.8, /*created_at=*/0, kNow);
    EXPECT_EQ(m.age_days, 0);
    EXPECT_DOUBLE_EQ(m.decay_factor, 1.0);
}

TEST(MemoryScorerScore, NegativeRawScore_ClampedToZero) {
    MemoryScorer s(DefaultConfig());
    auto m = s.Score(15, "c", "fact", "active", -0.5, CreatedAtAgedDays(0), kNow);
    EXPECT_DOUBLE_EQ(m.final_score, 0.0);
}

TEST(MemoryScorerScore, ZeroRawScore) {
    MemoryScorer s(DefaultConfig());
    auto m = s.Score(16, "c", "event", "active", 0.0, CreatedAtAgedDays(10), kNow);
    EXPECT_DOUBLE_EQ(m.final_score, 0.0);
}

TEST(MemoryScorerScore, CustomLambda) {
    MemoryDecayConfig c = DefaultConfig();
    c.lambda = 0.05;
    MemoryScorer s(c);
    auto m = s.Score(17, "c", "event", "active", 0.8, CreatedAtAgedDays(30), kNow);
    EXPECT_NEAR(m.decay_factor, 0.223, 1e-3);  // exp(-0.05*30)
    EXPECT_NEAR(m.final_score, 0.179, 1e-3);
}

TEST(MemoryScorerScore, MinScoreFloor) {
    MemoryDecayConfig c = DefaultConfig();
    c.min_score = 0.1;
    MemoryScorer s(c);
    // 365d event would decay to ~0.026, clamped up to the 0.1 floor.
    auto m = s.Score(18, "c", "event", "active", 0.8, CreatedAtAgedDays(365), kNow);
    EXPECT_DOUBLE_EQ(m.decay_factor, 0.1);
    EXPECT_NEAR(m.final_score, 0.08, 1e-9);
}

TEST(MemoryScorerScore, LambdaZero_AllImmune) {
    MemoryDecayConfig c = DefaultConfig();
    c.lambda = 0.0;
    MemoryScorer s(c);
    auto m = s.Score(19, "c", "event", "active", 0.8, CreatedAtAgedDays(365), kNow);
    EXPECT_DOUBLE_EQ(m.decay_factor, 1.0);  // exp(0) = 1
    EXPECT_DOUBLE_EQ(m.final_score, 0.8);
}

TEST(MemoryScorerScore, CandidateOverloadEquivalent) {
    MemoryScorer s(DefaultConfig());
    auto cand = MakeCandidate(20, "event", "active", 0.8, 30);
    auto direct = s.Score(20, cand.content, "event", "active", 0.8, cand.created_at, kNow);
    auto via = s.Score(cand, kNow);
    EXPECT_DOUBLE_EQ(via.final_score, direct.final_score);
    EXPECT_EQ(via.content, direct.content);
}

// ---------------------------------------------------------------------------
// IsDecayImmune / ComputeDecayFactor / GetConfig
// ---------------------------------------------------------------------------

TEST(MemoryScorerImmune, ImmuneTypes) {
    MemoryScorer s(DefaultConfig());
    EXPECT_TRUE(s.IsDecayImmune("fact"));
    EXPECT_TRUE(s.IsDecayImmune("preference"));
    EXPECT_FALSE(s.IsDecayImmune("event"));
    EXPECT_FALSE(s.IsDecayImmune(""));
    EXPECT_FALSE(s.IsDecayImmune("opinion"));
}

TEST(MemoryScorerImmune, ComputeDecayFactorBoundaries) {
    MemoryScorer s(DefaultConfig());
    EXPECT_DOUBLE_EQ(s.ComputeDecayFactor(0), 1.0);
    EXPECT_DOUBLE_EQ(s.ComputeDecayFactor(-5), 1.0);
    EXPECT_NEAR(s.ComputeDecayFactor(30), 0.741, 1e-3);
}

TEST(MemoryScorerImmune, GetConfigReflectsInput) {
    MemoryDecayConfig c = DefaultConfig();
    c.lambda = 0.02;
    c.min_score = 0.05;
    c.llm_available = true;
    MemoryScorer s(c);
    EXPECT_DOUBLE_EQ(s.GetConfig().lambda, 0.02);
    EXPECT_DOUBLE_EQ(s.GetConfig().min_score, 0.05);
    EXPECT_TRUE(s.GetConfig().llm_available);
}

// ---------------------------------------------------------------------------
// ScoreAndRank: filter + rank + truncate
// ---------------------------------------------------------------------------

TEST(MemoryScorerRank, MixedTypes_FactRanksAboveDecayedEvent) {
    MemoryScorer s(DefaultConfig());
    std::vector<MemoryCandidate> cands = {
        MakeCandidate(1, "fact", "active", 0.80, 90),        // final 0.80 (immune)
        MakeCandidate(2, "fact", "active", 0.75, 5),         // final 0.75 (immune)
        MakeCandidate(3, "event", "active", 0.90, 90),       // final ~0.366
        MakeCandidate(4, "event", "active", 0.88, 30),       // final ~0.652
        MakeCandidate(5, "event", "active", 0.70, 1),        // final ~0.693
        MakeCandidate(6, "fact", "invalidated", 0.99, 0),    // filtered out
    };
    auto out = s.ScoreAndRank(cands, kNow, /*top_k=*/10);
    ASSERT_EQ(out.size(), 5u);  // invalidated excluded
    // Top result is the fact at 0.80.
    EXPECT_EQ(out[0].block_id, 1u);
    EXPECT_DOUBLE_EQ(out[0].final_score, 0.80);
    // Sorted strictly descending by final_score.
    for (size_t i = 1; i < out.size(); ++i) {
        EXPECT_GE(out[i - 1].final_score, out[i].final_score);
    }
}

TEST(MemoryScorerRank, TopKTruncation) {
    MemoryScorer s(DefaultConfig());
    std::vector<MemoryCandidate> cands;
    for (int i = 0; i < 10; ++i) {
        cands.push_back(MakeCandidate(static_cast<uint64_t>(i + 1), "event",
                                      "active", 0.5 + 0.01 * i, 10));
    }
    auto out = s.ScoreAndRank(cands, kNow, /*top_k=*/3);
    EXPECT_EQ(out.size(), 3u);
}

TEST(MemoryScorerRank, StableSortOnTie) {
    MemoryScorer s(DefaultConfig());
    // Two facts with identical raw -> identical final; original order kept.
    std::vector<MemoryCandidate> cands = {
        MakeCandidate(100, "fact", "active", 0.5, 0),
        MakeCandidate(200, "fact", "active", 0.5, 0),
    };
    auto out = s.ScoreAndRank(cands, kNow, 10);
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0].block_id, 100u);
    EXPECT_EQ(out[1].block_id, 200u);
}

TEST(MemoryScorerRank, EmptyCandidates) {
    MemoryScorer s(DefaultConfig());
    auto out = s.ScoreAndRank({}, kNow, 10);
    EXPECT_TRUE(out.empty());
}

TEST(MemoryScorerRank, TopKZero_ReturnsEmpty) {
    MemoryScorer s(DefaultConfig());
    std::vector<MemoryCandidate> cands = {MakeCandidate(1, "fact", "active", 0.8, 0)};
    auto out = s.ScoreAndRank(cands, kNow, /*top_k=*/0);
    EXPECT_TRUE(out.empty());
}

TEST(MemoryScorerRank, TopKNegative_ReturnsEmpty) {
    MemoryScorer s(DefaultConfig());
    std::vector<MemoryCandidate> cands = {MakeCandidate(1, "fact", "active", 0.8, 0)};
    auto out = s.ScoreAndRank(cands, kNow, /*top_k=*/-3);
    EXPECT_TRUE(out.empty());
}

TEST(MemoryScorerRank, TopKLargerThanCandidates_ReturnsAll) {
    MemoryScorer s(DefaultConfig());
    std::vector<MemoryCandidate> cands = {
        MakeCandidate(1, "fact", "active", 0.8, 0),
        MakeCandidate(2, "event", "active", 0.7, 5),
    };
    auto out = s.ScoreAndRank(cands, kNow, /*top_k=*/100);
    EXPECT_EQ(out.size(), 2u);
}

TEST(MemoryScorerRank, IncludeInvalidatedFalse) {
    MemoryScorer s(DefaultConfig());
    std::vector<MemoryCandidate> cands = {
        MakeCandidate(1, "fact", "active", 0.8, 0),
        MakeCandidate(2, "event", "active", 0.7, 5),
        MakeCandidate(3, "fact", "invalidated", 0.9, 0),
        MakeCandidate(4, "event", "invalidated", 0.95, 0),
    };
    auto out = s.ScoreAndRank(cands, kNow, 10, /*include_invalidated=*/false);
    ASSERT_EQ(out.size(), 2u);
    for (const auto& m : out) {
        EXPECT_NE(m.status, "invalidated");
    }
}

TEST(MemoryScorerRank, IncludeInvalidatedTrue) {
    MemoryScorer s(DefaultConfig());
    std::vector<MemoryCandidate> cands = {
        MakeCandidate(1, "fact", "active", 0.80, 0),
        MakeCandidate(2, "event", "active", 0.70, 5),
        MakeCandidate(3, "fact", "invalidated", 0.90, 0),
        MakeCandidate(4, "event", "invalidated", 0.95, 0),
    };
    auto out = s.ScoreAndRank(cands, kNow, 10, /*include_invalidated=*/true);
    ASSERT_EQ(out.size(), 4u);  // invalidated retained
    // Highest final is the invalidated event at age 0 (0.95, decay 1.0),
    // ahead of the invalidated fact (0.90, immune). Both invalidated rows
    // participate in ranking because include_invalidated=true.
    EXPECT_EQ(out[0].block_id, 4u);
    EXPECT_DOUBLE_EQ(out[0].final_score, 0.95);
    EXPECT_EQ(out[1].block_id, 3u);
    EXPECT_DOUBLE_EQ(out[1].final_score, 0.90);
    for (size_t i = 1; i < out.size(); ++i) {
        EXPECT_GE(out[i - 1].final_score, out[i].final_score);
    }
}

TEST(MemoryScorerRank, DefaultIncludeInvalidatedIsFalse) {
    MemoryScorer s(DefaultConfig());
    std::vector<MemoryCandidate> cands = {
        MakeCandidate(1, "fact", "active", 0.8, 0),
        MakeCandidate(2, "fact", "invalidated", 0.9, 0),
    };
    // No include_invalidated argument -> defaults to false (strict filter).
    auto out = s.ScoreAndRank(cands, kNow, 10);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].block_id, 1u);
}

// ---------------------------------------------------------------------------
// lock: unknown memory_type emits a WARN containing "Unknown memory_type"
// ---------------------------------------------------------------------------

TEST(MemoryScorerLog, UnknownType_LogsWarn) {
    std::ostringstream oss;
    auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(oss);
    auto captured = std::make_shared<spdlog::logger>("memory_decay_test_capture", sink);
    captured->set_level(spdlog::level::trace);
    auto prev = spdlog::default_logger();
    spdlog::set_default_logger(captured);

    {
        MemoryScorer s(DefaultConfig());
        s.IsDecayImmune("opinion");
    }

    spdlog::set_default_logger(prev);  // restore for other tests
    // Drop the capture logger from spdlog's global registry. set_default_logger
    // also registered it under its name; without this drop the sink (which holds a
    // reference to the stack-local oss destroyed at function exit) lingers in the
    // registry, and the flush_every() background thread's flush_all() later
    // dereferences the dead ostream -> use-after-free / SIGSEGV.
    spdlog::drop("memory_decay_test_capture");
    const std::string logged = oss.str();
    EXPECT_NE(logged.find("Unknown memory_type"), std::string::npos) << logged;
    EXPECT_NE(logged.find("opinion"), std::string::npos) << logged;
}

TEST(MemoryScorerLog, KnownTypes_DoNotWarn) {
    std::ostringstream oss;
    auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(oss);
    auto captured = std::make_shared<spdlog::logger>("memory_decay_test_capture2", sink);
    captured->set_level(spdlog::level::trace);
    auto prev = spdlog::default_logger();
    spdlog::set_default_logger(captured);

    {
        MemoryScorer s(DefaultConfig());
        s.IsDecayImmune("event");
        s.IsDecayImmune("");
        s.IsDecayImmune("fact");
        s.IsDecayImmune("preference");
    }

    spdlog::set_default_logger(prev);
    spdlog::drop("memory_decay_test_capture2");  // see UnknownType_LogsWarn: avoid dead-sink UAF
    EXPECT_EQ(oss.str().find("Unknown memory_type"), std::string::npos) << oss.str();
}

// ---------------------------------------------------------------------------
// Struct default sanity
// ---------------------------------------------------------------------------

TEST(MemoryScorerStructs, DefaultConfigValues) {
    MemoryDecayConfig c;
    EXPECT_DOUBLE_EQ(c.lambda, 0.01);
    EXPECT_DOUBLE_EQ(c.min_score, 0.0);
    EXPECT_FALSE(c.llm_available);
}

TEST(MemoryScorerStructs, ScoredMemoryDefaults) {
    ScoredMemory m;
    EXPECT_EQ(m.block_id, 0u);
    EXPECT_DOUBLE_EQ(m.raw_score, 0.0);
    EXPECT_DOUBLE_EQ(m.decay_factor, 0.0);
    EXPECT_DOUBLE_EQ(m.final_score, 0.0);
    EXPECT_EQ(m.age_days, 0);
}

}  // namespace
}  // namespace cortrix
