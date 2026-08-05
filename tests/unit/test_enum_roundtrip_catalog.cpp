#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "cortrix/catalog/catalog_types.h"

// Exhaustive round-trip matrices for the catalog state/type enums
// (src/catalog/catalog_types.cpp). UnitState + IndexType both expose
// ToString + a *FromString reverse, and BOTH document "unknown -> default":
//   UnitStateFromString: unknown -> kActive  (Phase 1 conservative)
//   IndexTypeFromString: unknown -> kHnsw
// Suite names unique with RtCatalog* / UnitState* / IndexType* prefixes.
namespace cortrix::catalog {
namespace {

// ----------------------------------------------------------------------------
// UnitState: 5 values, ToString <-> UnitStateFromString.
// ----------------------------------------------------------------------------
const std::vector<UnitState>& AllUnitStates() {
    static const std::vector<UnitState> v = {
        UnitState::kActive, UnitState::kSealing, UnitState::kSealed,
        UnitState::kArchived, UnitState::kMigrating};
    return v;
}

class RtUnitStateMatrix : public ::testing::TestWithParam<UnitState> {};

TEST_P(RtUnitStateMatrix, ToStringFromStringRoundTrip) {
    const UnitState state = GetParam();
    const std::string token = ToString(state);
    EXPECT_FALSE(token.empty());
    EXPECT_EQ(UnitStateFromString(token), state)
        << "round-trip mismatch for token: " << token;
}

TEST_P(RtUnitStateMatrix, TokenLowercaseAscii) {
    const std::string token = ToString(GetParam());
    for (char c : token) {
        EXPECT_TRUE((c >= 'a' && c <= 'z') || c == '_')
            << "non-lowercase token char in: " << token;
    }
}

INSTANTIATE_TEST_SUITE_P(AllValues, RtUnitStateMatrix,
                         ::testing::ValuesIn(AllUnitStates()));

TEST(RtUnitStateTokens, ExactSchemaStrings) {
    EXPECT_STREQ(ToString(UnitState::kActive), "active");
    EXPECT_STREQ(ToString(UnitState::kSealing), "sealing");
    EXPECT_STREQ(ToString(UnitState::kSealed), "sealed");
    EXPECT_STREQ(ToString(UnitState::kArchived), "archived");
    EXPECT_STREQ(ToString(UnitState::kMigrating), "migrating");
}

// Unknown / empty / wrong-case -> kActive (documented Phase 1 conservative default).
class RtUnitStateUnknownMatrix : public ::testing::TestWithParam<std::string> {};

TEST_P(RtUnitStateUnknownMatrix, UnknownDefaultsToActive) {
    EXPECT_EQ(UnitStateFromString(GetParam()), UnitState::kActive)
        << "unexpected non-default for: " << GetParam();
}

INSTANTIATE_TEST_SUITE_P(Unknowns, RtUnitStateUnknownMatrix,
                         ::testing::Values(std::string(""), "ACTIVE", "Sealed",
                                           "deleted", "hnsw", "active "));

// ----------------------------------------------------------------------------
// IndexType: 3 values, ToString <-> IndexTypeFromString. unknown -> kHnsw.
// ----------------------------------------------------------------------------
const std::vector<IndexType>& AllIndexTypes() {
    static const std::vector<IndexType> v = {
        IndexType::kHnsw, IndexType::kBruteForce, IndexType::kQuantizedHnsw};
    return v;
}

class RtIndexTypeMatrix : public ::testing::TestWithParam<IndexType> {};

TEST_P(RtIndexTypeMatrix, ToStringFromStringRoundTrip) {
    const IndexType type = GetParam();
    const std::string token = ToString(type);
    EXPECT_FALSE(token.empty());
    EXPECT_EQ(IndexTypeFromString(token), type)
        << "round-trip mismatch for token: " << token;
}

INSTANTIATE_TEST_SUITE_P(AllValues, RtIndexTypeMatrix,
                         ::testing::ValuesIn(AllIndexTypes()));

TEST(RtIndexTypeTokens, ExactSchemaStrings) {
    EXPECT_STREQ(ToString(IndexType::kHnsw), "hnsw");
    EXPECT_STREQ(ToString(IndexType::kBruteForce), "bruteforce");
    EXPECT_STREQ(ToString(IndexType::kQuantizedHnsw), "quantized_hnsw");
}

class RtIndexTypeUnknownMatrix : public ::testing::TestWithParam<std::string> {};

TEST_P(RtIndexTypeUnknownMatrix, UnknownDefaultsToHnsw) {
    EXPECT_EQ(IndexTypeFromString(GetParam()), IndexType::kHnsw)
        << "unexpected non-default for: " << GetParam();
}

INSTANTIATE_TEST_SUITE_P(Unknowns, RtIndexTypeUnknownMatrix,
                         ::testing::Values(std::string(""), "HNSW", "ivf",
                                           "BruteForce", "flat", "hnsw "));

}  // namespace
}  // namespace cortrix::catalog
