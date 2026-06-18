#include <gtest/gtest.h>

#include <map>
#include <string>

#include "cortrix/query/live_single_unit_executor.h"

// FlattenMetadataIntoMap — the FiQA result-identity fix. The live per-NS executor
// must surface a block's metadata_json fields as TOP-LEVEL result-metadata keys
// (so the cross-NS runner can match qrels on, e.g., beir_corpus_id), NOT bury the
// whole JSON blob under a single "metadata_json" key.
namespace cortrix::query {
namespace {

// Top-level identity fields (beir_corpus_id) become addressable map keys with the
// raw string value (no surrounding quotes), and the whole blob is NOT nested under
// a "metadata_json" key (the bug).
TEST(LiveMetadataFlattenTest, TopLevelFieldsAreFlattened) {
    std::map<std::string, std::string> out;
    FlattenMetadataIntoMap(
        R"({"beir_corpus_id":"FiQA_42","source":"train"})", out);

    EXPECT_EQ(out["beir_corpus_id"], "FiQA_42");  // raw id, not "\"FiQA_42\""
    EXPECT_EQ(out["source"], "train");
    EXPECT_EQ(out.count("metadata_json"), 0u);  // no opaque blob key (the bug)
}

// Non-string scalars / arrays / nested objects survive as serialized JSON text in
// the string→string map (dump()), so no information is lost.
TEST(LiveMetadataFlattenTest, NonStringValuesSerializeViaDump) {
    std::map<std::string, std::string> out;
    FlattenMetadataIntoMap(
        R"({"page":7,"score":0.5,"tags":["a","b"],"nested":{"k":"v"}})", out);

    EXPECT_EQ(out["page"], "7");
    EXPECT_EQ(out["score"], "0.5");
    EXPECT_EQ(out["tags"], R"(["a","b"])");
    EXPECT_EQ(out["nested"], R"({"k":"v"})");
}

// Invalid / empty / non-object JSON is tolerated (no throw) and leaves the map's
// existing entries untouched (post_filter.cpp parity).
TEST(LiveMetadataFlattenTest, InvalidJsonIsToleratedAndNonDestructive) {
    std::map<std::string, std::string> out;
    out["via_path"] = "doc_summary";  // a pre-existing provenance key

    EXPECT_NO_THROW(FlattenMetadataIntoMap("", out));
    EXPECT_NO_THROW(FlattenMetadataIntoMap("{not valid json", out));
    EXPECT_NO_THROW(FlattenMetadataIntoMap("[1,2,3]", out));  // array, not object

    EXPECT_EQ(out.size(), 1u);
    EXPECT_EQ(out["via_path"], "doc_summary");  // untouched
}

// A later top-level field overrides nothing pre-existing unless keys collide; on a
// key collision the metadata_json value wins (it is the document's own field).
TEST(LiveMetadataFlattenTest, MetadataFieldOverridesOnKeyCollision) {
    std::map<std::string, std::string> out;
    out["source"] = "seed";
    FlattenMetadataIntoMap(R"({"source":"doc"})", out);
    EXPECT_EQ(out["source"], "doc");  // doc's own metadata field wins
}

}  // namespace
}  // namespace cortrix::query
