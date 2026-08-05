#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "cortrix/memory_scorer.h"

// Value-matrix breadth coverage for MemoryScorer decay + ScoreAndRank.
//
// Real behavior (memory_scorer.h / src/memory/memory_scorer.cpp, verified):
//   - Defaults: MemoryDecayConfig.lambda=0.01, min_score=0.0.
//   - ComputeDecayFactor(age_days): age<=0 -> 1.0; else max(exp(-lambda*age),
//     min_score).
//   - IsDecayImmune: {"fact","preference"} -> true (factor 1.0). "event"/"" and
//     unknown -> false (unknown also WARN-logs).
//   - Score: age_days = (created>0 && now>created) ? (now-created)/86400 : 0.
//     final_score = max(raw*decay, 0)  (negative clamped to 0).
//   - ScoreAndRank: drops status=="invalidated" unless include_invalidated;
//     stable_sort by final_score DESC (ties keep insertion order); top_k<=0 ->
//     EMPTY (incl. negatives); top_k>size -> all.
//
// All suite + fixture names globally unique (ScorerDecayMatrix* prefix).

namespace cortrix {
namespace {

constexpr int64_t kDay = 86400;

MemoryScorer MakeScorer(double lambda, double min_score = 0.0) {
    MemoryDecayConfig cfg;
    cfg.lambda = lambda;
    cfg.min_score = min_score;
    return MemoryScorer(cfg);
}

MemoryCandidate MakeCand(uint64_t id, const std::string& type,
                         const std::string& status, double raw,
                         int64_t created_at) {
    MemoryCandidate c;
    c.block_id = id;
    c.content = "c";
    c.memory_type = type;
    c.status = status;
    c.raw_score = raw;
    c.created_at = created_at;
    return c;
}

// ==========================================================================
// Default-config constants are what the design locks (lambda=0.01).
// ==========================================================================
TEST(ScorerDecayMatrixDefaults, DefaultLambdaAndFloor) {
    MemoryDecayConfig cfg;  // defaults
    EXPECT_DOUBLE_EQ(cfg.lambda, 0.01);
    EXPECT_DOUBLE_EQ(cfg.min_score, 0.0);
    EXPECT_FALSE(cfg.llm_available);
}

// ==========================================================================
// ComputeDecayFactor over age x lambda matrix.
// ==========================================================================
struct ScorerDecayMatrixFactorCase {
    double lambda;
    int64_t age_days;
};

class ScorerDecayMatrixFactor
    : public ::testing::TestWithParam<ScorerDecayMatrixFactorCase> {};

TEST_P(ScorerDecayMatrixFactor, MatchesFormula) {
    const auto& tc = GetParam();
    MemoryScorer s = MakeScorer(tc.lambda);
    double got = s.ComputeDecayFactor(tc.age_days);
    if (tc.age_days <= 0) {
        EXPECT_DOUBLE_EQ(got, 1.0);
    } else {
        double expected =
            std::exp(-tc.lambda * static_cast<double>(tc.age_days));
        // min_score floor = 0, so no clamping in this matrix.
        EXPECT_DOUBLE_EQ(got, expected);
        EXPECT_GE(got, 0.0);
        EXPECT_LE(got, 1.0);
    }
}

INSTANTIATE_TEST_SUITE_P(
    Matrix, ScorerDecayMatrixFactor,
    ::testing::Values(
        // age boundaries
        ScorerDecayMatrixFactorCase{0.01, 0},
        ScorerDecayMatrixFactorCase{0.01, 1},
        ScorerDecayMatrixFactorCase{0.01, 70},   // ~half-life
        ScorerDecayMatrixFactorCase{0.01, 365},
        ScorerDecayMatrixFactorCase{0.01, -1},   // negative -> 1.0
        ScorerDecayMatrixFactorCase{0.01, -1000000},
        ScorerDecayMatrixFactorCase{0.01,
                                    std::numeric_limits<int64_t>::max()},
        // lambda boundaries
        ScorerDecayMatrixFactorCase{0.0, 100},   // lambda 0 -> factor 1.0
        ScorerDecayMatrixFactorCase{0.001, 100},
        ScorerDecayMatrixFactorCase{0.5, 10},
        ScorerDecayMatrixFactorCase{1.0, 5},
        ScorerDecayMatrixFactorCase{0.1, 1}));

// lambda==0 => exp(0) == 1.0 for ANY positive age.
TEST(ScorerDecayMatrixFactor, ZeroLambdaNeverDecays) {
    MemoryScorer s = MakeScorer(0.0);
    EXPECT_DOUBLE_EQ(s.ComputeDecayFactor(1), 1.0);
    EXPECT_DOUBLE_EQ(s.ComputeDecayFactor(100000), 1.0);
}

// Huge age + positive lambda underflows toward 0 but stays >= min_score floor.
TEST(ScorerDecayMatrixFactor, HugeAgeFloorsAtMinScore) {
    MemoryScorer s = MakeScorer(0.01, /*min_score=*/0.25);
    double got = s.ComputeDecayFactor(std::numeric_limits<int64_t>::max());
    EXPECT_DOUBLE_EQ(got, 0.25);  // clamped to floor
}

TEST(ScorerDecayMatrixFactor, LargeLambdaFloorsAtMinScore) {
    MemoryScorer s = MakeScorer(100.0, /*min_score=*/0.1);
    EXPECT_DOUBLE_EQ(s.ComputeDecayFactor(1000), 0.1);
}

// ==========================================================================
// IsDecayImmune matrix.
// ==========================================================================
struct ScorerDecayMatrixImmuneCase {
    std::string type;
    bool immune;
};

class ScorerDecayMatrixImmune
    : public ::testing::TestWithParam<ScorerDecayMatrixImmuneCase> {};

TEST_P(ScorerDecayMatrixImmune, ClassifiesType) {
    const auto& tc = GetParam();
    MemoryScorer s = MakeScorer(0.01);
    EXPECT_EQ(s.IsDecayImmune(tc.type), tc.immune);
}

INSTANTIATE_TEST_SUITE_P(
    Matrix, ScorerDecayMatrixImmune,
    ::testing::Values(
        ScorerDecayMatrixImmuneCase{"fact", true},
        ScorerDecayMatrixImmuneCase{"preference", true},
        ScorerDecayMatrixImmuneCase{"event", false},
        ScorerDecayMatrixImmuneCase{"", false},
        ScorerDecayMatrixImmuneCase{"unknown", false},
        ScorerDecayMatrixImmuneCase{"Fact", false},   // case-sensitive
        ScorerDecayMatrixImmuneCase{"FACT", false},
        ScorerDecayMatrixImmuneCase{"facts", false},
        ScorerDecayMatrixImmuneCase{"belief", false}));

// ==========================================================================
// Score(): immune types ignore decay; event decays; final clamped >= 0.
// ==========================================================================
TEST(ScorerDecayMatrixScore, ImmuneTypesUseFactorOne) {
    MemoryScorer s = MakeScorer(0.01);
    const int64_t now = 1000 * kDay;
    const int64_t created = 0 * kDay + 1;  // very old
    for (const std::string& t : {std::string("fact"), std::string("preference")}) {
        ScoredMemory m = s.Score(1, "x", t, "active", 0.8, created, now);
        EXPECT_DOUBLE_EQ(m.decay_factor, 1.0) << t;
        EXPECT_DOUBLE_EQ(m.final_score, 0.8) << t;
    }
}

TEST(ScorerDecayMatrixScore, EventDecaysByAge) {
    MemoryScorer s = MakeScorer(0.01);
    const int64_t created = 1;
    const int64_t now = created + 100 * kDay;  // 100 days old
    ScoredMemory m = s.Score(1, "x", "event", "active", 1.0, created, now);
    EXPECT_EQ(m.age_days, 100);
    EXPECT_NEAR(m.decay_factor, std::exp(-0.01 * 100.0), 1e-9);
    EXPECT_NEAR(m.final_score, std::exp(-0.01 * 100.0), 1e-9);
}

// age_days computation matrix: created/now relationship.
struct ScorerDecayMatrixAgeCase {
    int64_t created_at;
    int64_t now;
    int64_t expect_age_days;
};

class ScorerDecayMatrixAge
    : public ::testing::TestWithParam<ScorerDecayMatrixAgeCase> {};

TEST_P(ScorerDecayMatrixAge, AgeDaysComputation) {
    const auto& tc = GetParam();
    MemoryScorer s = MakeScorer(0.01);
    ScoredMemory m =
        s.Score(1, "x", "event", "active", 1.0, tc.created_at, tc.now);
    EXPECT_EQ(m.age_days, tc.expect_age_days);
}

INSTANTIATE_TEST_SUITE_P(
    Matrix, ScorerDecayMatrixAge,
    ::testing::Values(
        ScorerDecayMatrixAgeCase{kDay, kDay, 0},            // now==created
        ScorerDecayMatrixAgeCase{kDay, kDay + 1, 0},        // <1 day
        ScorerDecayMatrixAgeCase{kDay, 2 * kDay, 1},        // exactly 1 day
        ScorerDecayMatrixAgeCase{kDay, kDay + kDay * 5, 5},
        ScorerDecayMatrixAgeCase{kDay, kDay - 1, 0},        // now<created -> 0
        ScorerDecayMatrixAgeCase{0, 1000 * kDay, 0},        // created==0 -> 0
        ScorerDecayMatrixAgeCase{-5, 1000 * kDay, 0},       // created<=0 -> 0
        ScorerDecayMatrixAgeCase{1, 1 + 365 * kDay, 365}));

// Negative raw_score clamps final to 0 (even for immune types).
class ScorerDecayMatrixNegRaw : public ::testing::TestWithParam<double> {};

TEST_P(ScorerDecayMatrixNegRaw, NegativeRawClampedToZero) {
    const double raw = GetParam();
    MemoryScorer s = MakeScorer(0.01);
    ScoredMemory m = s.Score(1, "x", "fact", "active", raw, 1, 1 + 10 * kDay);
    EXPECT_DOUBLE_EQ(m.final_score, 0.0);
}

INSTANTIATE_TEST_SUITE_P(Matrix, ScorerDecayMatrixNegRaw,
                         ::testing::Values(-0.0001, -1.0, -1e9,
                                           -std::numeric_limits<double>::max()));

TEST(ScorerDecayMatrixScore, ZeroRawGivesZeroFinal) {
    MemoryScorer s = MakeScorer(0.01);
    ScoredMemory m = s.Score(1, "x", "event", "active", 0.0, 1, 1 + 10 * kDay);
    EXPECT_DOUBLE_EQ(m.final_score, 0.0);
}

// ==========================================================================
// ScoreAndRank top_k boundary matrix.
// ==========================================================================
struct ScorerDecayMatrixTopKCase {
    int top_k;
    size_t expect_size;  // out of 5 candidates
};

class ScorerDecayMatrixTopK
    : public ::testing::TestWithParam<ScorerDecayMatrixTopKCase> {};

TEST_P(ScorerDecayMatrixTopK, TruncatesCorrectly) {
    const auto& tc = GetParam();
    MemoryScorer s = MakeScorer(0.01);
    std::vector<MemoryCandidate> cands;
    for (uint64_t i = 0; i < 5; ++i) {
        cands.push_back(MakeCand(i, "fact", "active",
                                 0.9 - 0.1 * static_cast<double>(i), 1));
    }
    std::vector<ScoredMemory> out = s.ScoreAndRank(cands, 1000 * kDay, tc.top_k);
    EXPECT_EQ(out.size(), tc.expect_size);
}

INSTANTIATE_TEST_SUITE_P(
    Matrix, ScorerDecayMatrixTopK,
    ::testing::Values(
        ScorerDecayMatrixTopKCase{0, 0},     // top_k 0 -> empty
        ScorerDecayMatrixTopKCase{-1, 0},    // negative -> empty
        ScorerDecayMatrixTopKCase{-100, 0},
        ScorerDecayMatrixTopKCase{1, 1},
        ScorerDecayMatrixTopKCase{3, 3},
        ScorerDecayMatrixTopKCase{5, 5},     // == size
        ScorerDecayMatrixTopKCase{6, 5},     // oversize -> all
        ScorerDecayMatrixTopKCase{1000000, 5}));

// ==========================================================================
// ScoreAndRank ordering: DESC by final_score, stable on ties (insertion order).
// ==========================================================================
TEST(ScorerDecayMatrixRank, DescendingByFinalScore) {
    MemoryScorer s = MakeScorer(0.01);
    std::vector<MemoryCandidate> cands = {
        MakeCand(10, "fact", "active", 0.3, 1),
        MakeCand(20, "fact", "active", 0.9, 1),
        MakeCand(30, "fact", "active", 0.6, 1),
    };
    std::vector<ScoredMemory> out = s.ScoreAndRank(cands, 1000 * kDay, 10);
    ASSERT_EQ(out.size(), 3u);
    EXPECT_EQ(out[0].block_id, 20u);  // 0.9
    EXPECT_EQ(out[1].block_id, 30u);  // 0.6
    EXPECT_EQ(out[2].block_id, 10u);  // 0.3
    EXPECT_GE(out[0].final_score, out[1].final_score);
    EXPECT_GE(out[1].final_score, out[2].final_score);
}

TEST(ScorerDecayMatrixRank, StableTieKeepsInsertionOrder) {
    MemoryScorer s = MakeScorer(0.01);
    // All same raw + immune type -> identical final_score -> ties.
    std::vector<MemoryCandidate> cands;
    for (uint64_t i = 0; i < 6; ++i) {
        cands.push_back(MakeCand(100 + i, "fact", "active", 0.5, 1));
    }
    std::vector<ScoredMemory> out = s.ScoreAndRank(cands, 1000 * kDay, 10);
    ASSERT_EQ(out.size(), 6u);
    for (uint64_t i = 0; i < 6; ++i) {
        EXPECT_EQ(out[i].block_id, 100 + i)
            << "stable_sort must preserve insertion order on ties";
    }
}

// ==========================================================================
// include_invalidated filter matrix.
// ==========================================================================
struct ScorerDecayMatrixInvalidatedCase {
    bool include_invalidated;
    size_t expect_size;
};

class ScorerDecayMatrixInvalidated
    : public ::testing::TestWithParam<ScorerDecayMatrixInvalidatedCase> {};

TEST_P(ScorerDecayMatrixInvalidated, FilterBehavior) {
    const auto& tc = GetParam();
    MemoryScorer s = MakeScorer(0.01);
    std::vector<MemoryCandidate> cands = {
        MakeCand(1, "fact", "active", 0.9, 1),
        MakeCand(2, "fact", "invalidated", 0.8, 1),
        MakeCand(3, "fact", "tentative", 0.7, 1),
        MakeCand(4, "fact", "invalidated", 0.6, 1),
    };
    std::vector<ScoredMemory> out =
        s.ScoreAndRank(cands, 1000 * kDay, 100, tc.include_invalidated);
    EXPECT_EQ(out.size(), tc.expect_size);
    if (!tc.include_invalidated) {
        for (const auto& m : out) EXPECT_NE(m.status, "invalidated");
    }
}

INSTANTIATE_TEST_SUITE_P(
    Matrix, ScorerDecayMatrixInvalidated,
    ::testing::Values(
        ScorerDecayMatrixInvalidatedCase{false, 2},  // drops 2 invalidated
        ScorerDecayMatrixInvalidatedCase{true, 4}));

// "invalidated" status does not decay-immunize: still filtered regardless of
// type; non-invalidated statuses are all kept.
TEST(ScorerDecayMatrixInvalidated, OtherStatusesAlwaysKept) {
    MemoryScorer s = MakeScorer(0.01);
    std::vector<MemoryCandidate> cands = {
        MakeCand(1, "event", "active", 0.5, 1),
        MakeCand(2, "event", "tentative", 0.5, 1),
        MakeCand(3, "event", "", 0.5, 1),
    };
    std::vector<ScoredMemory> out = s.ScoreAndRank(cands, 1000 * kDay, 100);
    EXPECT_EQ(out.size(), 3u);
}

// Empty input -> empty output (no crash).
TEST(ScorerDecayMatrixRank, EmptyCandidatesEmptyResult) {
    MemoryScorer s = MakeScorer(0.01);
    std::vector<MemoryCandidate> cands;
    std::vector<ScoredMemory> out = s.ScoreAndRank(cands, 1000 * kDay, 10);
    EXPECT_TRUE(out.empty());
}

// Convenience overload Score(cand, now) parity with the 7-arg overload.
TEST(ScorerDecayMatrixScore, CandidateOverloadParity) {
    MemoryScorer s = MakeScorer(0.01);
    const int64_t now = 1 + 50 * kDay;
    MemoryCandidate c = MakeCand(9, "event", "active", 0.7, 1);
    ScoredMemory a = s.Score(c, now);
    ScoredMemory b =
        s.Score(9, "c", "event", "active", 0.7, 1, now);
    EXPECT_EQ(a.age_days, b.age_days);
    EXPECT_DOUBLE_EQ(a.decay_factor, b.decay_factor);
    EXPECT_DOUBLE_EQ(a.final_score, b.final_score);
}

}  // namespace
}  // namespace cortrix
