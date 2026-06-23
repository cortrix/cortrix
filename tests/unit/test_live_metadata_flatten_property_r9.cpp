// R9 robustness — property-based tests for FlattenMetadataIntoMap
// (src/query/live_single_unit_executor.cpp:41 — the FiQA result-identity fix).
//
// tests/scatter/test_live_metadata_flatten.cpp covers the fixed examples (string
// values verbatim, non-string via dump(), no opaque "metadata_json" key). This file
// is the property lane: the same flatten invariants must hold for ARBITRARY JSON
// objects rapidcheck builds, and — critically for a robustness sweep — flattening
// arbitrary / malformed / non-object metadata must NEVER throw (the catch-all is the
// load-bearing safety net on the live query path).
//
// FlattenMetadataIntoMap is a free function (declared in the header), pure modulo
// its out-param, with no I/O — an ideal property target. Standalone NEW file.
#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cortrix/query/live_single_unit_executor.h"

namespace cortrix::query {
namespace {

using json = nlohmann::json;

// A generator of VALID-UTF-8 strings (printable ASCII + a few multi-byte code
// points). The JSON-building generators below must use this rather than
// rc::gen::arbitrary<std::string>(): nlohmann::json::dump() THROWS type_error.316
// on a string holding invalid UTF-8, and these properties serialize their generated
// object via dump() in the TEST SETUP (before the function under test runs). The
// function's own robustness against arbitrary/invalid bytes is covered separately by
// NeverThrowsOnArbitraryBytes, which feeds raw arbitrary<std::string>() straight in.
rc::Gen<std::string> genUtf8String() {
    static const std::vector<std::string> kAtoms = {
        "a", "b", "Z", "0", "9", "_", "-", " ", ".", "/", ":", "key", "id",
        "é" /* é */, "中" /* U+4E2D */, "\U0001F600" /* 😀 */};
    return rc::gen::map(
        rc::gen::container<std::vector<std::size_t>>(
            rc::gen::inRange<std::size_t>(0, kAtoms.size())),
        [](const std::vector<std::size_t>& idxs) {
            std::string s;
            for (std::size_t i : idxs) s += kAtoms[i];
            return s;
        });
}

// Generate a "JSON value of arbitrary type" (1 level deep — enough to exercise the
// is_string() branch plus the dump() branch for scalars/arrays/objects).
rc::Gen<json> genJsonValue() {
    return rc::gen::oneOf(
        rc::gen::map(genUtf8String(), [](std::string s) { return json(s); }),
        rc::gen::map(rc::gen::arbitrary<int>(), [](int n) { return json(n); }),
        rc::gen::map(rc::gen::arbitrary<double>(), [](double d) { return json(d); }),
        rc::gen::map(rc::gen::arbitrary<bool>(), [](bool b) { return json(b); }),
        rc::gen::just(json(nullptr)),
        rc::gen::map(rc::gen::arbitrary<std::vector<int>>(),
                     [](std::vector<int> v) { return json(v); }),
        rc::gen::just(json({{"k", "v"}})));
}

// A flat JSON OBJECT with random (valid-UTF-8) string keys → random-typed values.
rc::Gen<json> genJsonObject() {
    return rc::gen::map(
        rc::gen::container<std::map<std::string, json>>(genUtf8String(), genJsonValue()),
        [](const std::map<std::string, json>& m) {
            json j = json::object();
            for (const auto& kv : m) j[kv.first] = kv.second;
            return j;
        });
}

// =====================================================================
// Safety: never throws on ANY input string (the catch-all guarantee).
// =====================================================================

// Arbitrary bytes (almost always invalid JSON) → no throw, and on a parse failure
// the map is left untouched (post_filter parity). We pre-seed the map and assert it
// is preserved when the input is not a JSON object.
RC_GTEST_PROP(LiveFlattenPropR9, NeverThrowsOnArbitraryBytes,
              (const std::string& raw)) {
    std::map<std::string, std::string> out;
    out["__sentinel__"] = "keep";
    // Must not throw for any byte string.
    FlattenMetadataIntoMap(raw, out);
    // The sentinel is never removed (flatten only inserts/overwrites parsed keys).
    RC_ASSERT(out["__sentinel__"] == "keep");
}

// Empty string is an explicit early-return: the map is completely unchanged.
RC_GTEST_PROP(LiveFlattenPropR9, EmptyInputLeavesMapUnchanged,
              (const std::map<std::string, std::string>& seed)) {
    std::map<std::string, std::string> out = seed;
    FlattenMetadataIntoMap("", out);
    RC_ASSERT(out == seed);
}

// A serialized JSON value that is NOT an object (scalar / array) → early return,
// map unchanged (only object-shaped metadata flattens).
RC_GTEST_PROP(LiveFlattenPropR9, NonObjectJsonLeavesMapUnchanged, ()) {
    // Build a non-object JSON value and serialize it.
    const json v = *rc::gen::oneOf(
        rc::gen::map(rc::gen::arbitrary<int>(), [](int n) { return json(n); }),
        rc::gen::map(genUtf8String(), [](std::string s) { return json(s); }),
        rc::gen::map(rc::gen::arbitrary<std::vector<int>>(),
                     [](std::vector<int> x) { return json(x); }),
        rc::gen::just(json(true)),
        rc::gen::just(json(nullptr)));
    const std::map<std::string, std::string> seed = {{"a", "1"}, {"b", "2"}};
    std::map<std::string, std::string> out = seed;
    FlattenMetadataIntoMap(v.dump(), out);
    RC_ASSERT(out == seed);
}

// =====================================================================
// Correctness invariants over arbitrary JSON OBJECTS.
// =====================================================================

// For any JSON object, EVERY top-level key appears in the output map exactly once,
// and the opaque "metadata_json" blob key is NEVER synthesized (the original bug).
RC_GTEST_PROP(LiveFlattenPropR9, EveryTopLevelKeyFlattened, ()) {
    const json obj = *genJsonObject();
    std::map<std::string, std::string> out;
    FlattenMetadataIntoMap(obj.dump(), out);

    // Same key set (the object's keys are all top-level, none nested under a blob).
    RC_ASSERT(out.size() == obj.size());
    for (auto it = obj.begin(); it != obj.end(); ++it) {
        RC_ASSERT(out.count(it.key()) == 1u);
    }
    // The bug sentinel: a "metadata_json" key only exists if the SOURCE object had
    // one literally — flatten never invents it.
    if (!obj.contains("metadata_json")) {
        RC_ASSERT(out.count("metadata_json") == 0u);
    }
}

// String values are stored VERBATIM (no surrounding quotes); every non-string value
// is stored as its JSON dump() text. This is the identity-preservation contract
// (beir_corpus_id must stay a raw id, not "\"id\"").
RC_GTEST_PROP(LiveFlattenPropR9, StringVerbatimNonStringDumped, ()) {
    const json obj = *genJsonObject();
    std::map<std::string, std::string> out;
    FlattenMetadataIntoMap(obj.dump(), out);

    for (auto it = obj.begin(); it != obj.end(); ++it) {
        const std::string& got = out[it.key()];
        if (it.value().is_string()) {
            RC_ASSERT(got == it.value().get<std::string>());  // verbatim, unquoted
        } else {
            RC_ASSERT(got == it.value().dump());               // serialized text
        }
    }
}

// Round-trip for a string-only object: flattening reproduces exactly the original
// key→value string map (a clean isomorphism on the all-string case, which is the
// common BEIR-metadata shape). Keys+values are valid-UTF-8 (so the setup dump()
// can't throw); the function's invalid-byte safety is covered by
// NeverThrowsOnArbitraryBytes.
RC_GTEST_PROP(LiveFlattenPropR9, StringOnlyObjectRoundTrips, ()) {
    const auto kv = *rc::gen::container<std::map<std::string, std::string>>(
        genUtf8String(), genUtf8String());
    json obj = json::object();
    for (const auto& p : kv) obj[p.first] = p.second;
    std::map<std::string, std::string> out;
    FlattenMetadataIntoMap(obj.dump(), out);
    RC_ASSERT(out == kv);
}

}  // namespace
}  // namespace cortrix::query
