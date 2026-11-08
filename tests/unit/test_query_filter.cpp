// QueryFilter parsing and document matching (tags, date_range) for POST /api/v1/query.

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "cortrix/query/query_filter.h"

namespace cortrix::query {
namespace {

using json = nlohmann::json;

TEST(QueryFilterParse, AbsentOrEmptyIsInactive) {
    QueryFilterSpec spec;
    ASSERT_TRUE(ParseQueryFilter(json(nullptr), &spec).ok());
    EXPECT_FALSE(spec.active());
    ASSERT_TRUE(ParseQueryFilter(json::object(), &spec).ok());
    EXPECT_FALSE(spec.active());
}

TEST(QueryFilterParse, TagsAndDateRange) {
    QueryFilterSpec spec;
    ASSERT_TRUE(ParseQueryFilter(json{{"tags", {"contract", "legal"}},
                                      {"date_range", {{"start", "2026-01-01"},
                                                      {"end", "2026-12-31"}}}},
                                 &spec)
                    .ok());
    EXPECT_TRUE(spec.active());
    EXPECT_EQ(spec.tags, (std::vector<std::string>{"contract", "legal"}));
    EXPECT_EQ(*spec.date_start, "2026-01-01");
    EXPECT_EQ(*spec.date_end, "2026-12-31");
    EXPECT_TRUE(spec.ignored_keys.empty());
}

TEST(QueryFilterParse, OpenEndedRange) {
    QueryFilterSpec spec;
    ASSERT_TRUE(ParseQueryFilter(json{{"date_range", {{"start", "2026-03-01"}}}}, &spec).ok());
    EXPECT_TRUE(spec.date_start.has_value());
    EXPECT_FALSE(spec.date_end.has_value());
}

TEST(QueryFilterParse, UnknownKeysAreRecordedNotApplied) {
    QueryFilterSpec spec;
    ASSERT_TRUE(ParseQueryFilter(json{{"block_type", {"FILE"}}, {"source_path", "a.md"}}, &spec)
                    .ok());
    EXPECT_FALSE(spec.active());
    EXPECT_EQ(spec.ignored_keys, (std::vector<std::string>{"block_type", "source_path"}));
}

TEST(QueryFilterParse, WrongTypesAreRejected) {
    QueryFilterSpec spec;
    EXPECT_FALSE(ParseQueryFilter(json("tags"), &spec).ok());
    EXPECT_FALSE(ParseQueryFilter(json{{"tags", "contract"}}, &spec).ok());
    EXPECT_FALSE(ParseQueryFilter(json{{"tags", {1, 2}}}, &spec).ok());
    EXPECT_FALSE(ParseQueryFilter(json{{"date_range", "2026"}}, &spec).ok());
    EXPECT_FALSE(ParseQueryFilter(json{{"date_range", {{"start", "2026-1-1"}}}}, &spec).ok());
    EXPECT_FALSE(ParseQueryFilter(json{{"date_range", {{"end", "2026-13-01"}}}}, &spec).ok());
    EXPECT_FALSE(ParseQueryFilter(
                     json{{"date_range", {{"start", "2026-05-01"}, {"end", "2026-04-01"}}}},
                     &spec)
                     .ok());
}

TEST(QueryFilterMatch, DateRangeIsInclusiveOnUtcDate) {
    QueryFilterSpec spec;
    spec.date_start = "2026-11-01";
    spec.date_end = "2026-11-05";
    EXPECT_TRUE(DocumentMatchesFilter(spec, "2026-11-01T00:00:00.000Z", ""));
    EXPECT_TRUE(DocumentMatchesFilter(spec, "2026-11-05T23:59:59.999Z", ""));
    EXPECT_FALSE(DocumentMatchesFilter(spec, "2026-10-31T23:59:59.999Z", ""));
    EXPECT_FALSE(DocumentMatchesFilter(spec, "2026-11-06T00:00:00.000Z", ""));
    EXPECT_FALSE(DocumentMatchesFilter(spec, "", ""));
}

TEST(QueryFilterMatch, TagsMatchAnyOfUploadMetadataTags) {
    QueryFilterSpec spec;
    spec.tags = {"contract", "invoice"};
    EXPECT_TRUE(DocumentMatchesFilter(spec, "", R"({"tags":["legal","contract"]})"));
    EXPECT_TRUE(DocumentMatchesFilter(spec, "", R"({"tags":"invoice"})"));
    EXPECT_FALSE(DocumentMatchesFilter(spec, "", R"({"tags":["legal"]})"));
    EXPECT_FALSE(DocumentMatchesFilter(spec, "", R"({"author":"x"})"));
    EXPECT_FALSE(DocumentMatchesFilter(spec, "", ""));
    EXPECT_FALSE(DocumentMatchesFilter(spec, "", "not json"));
}

TEST(QueryFilterMatch, BothConditionsMustHold) {
    QueryFilterSpec spec;
    spec.tags = {"contract"};
    spec.date_start = "2026-11-01";
    EXPECT_TRUE(DocumentMatchesFilter(spec, "2026-11-02T00:00:00Z", R"({"tags":["contract"]})"));
    EXPECT_FALSE(DocumentMatchesFilter(spec, "2026-10-02T00:00:00Z", R"({"tags":["contract"]})"));
    EXPECT_FALSE(DocumentMatchesFilter(spec, "2026-11-02T00:00:00Z", R"({"tags":["other"]})"));
}

}  // namespace
}  // namespace cortrix::query
