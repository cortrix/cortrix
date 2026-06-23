// R9 robustness — property-based tests for src/query/query_request.cpp.
//
// The R8 file (test_query_request_r8.cpp) enumerates the per-field FromJson guard
// arms (present-correct / present-wrong-type / not-object) one row at a time. This
// file is the property lane: it asserts the INVARIANTS those guards exist to
// uphold, over arbitrary JSON rapidcheck builds — most importantly the
// "lenient parse never crashes + a wrong-typed field always keeps its default"
// contract (the R8-verified `route`/`granularity` default = "auto").
//
// FromJson is pure (JSON in → struct out, always returns Ok for an object) and has
// no I/O, so it is an ideal property target. Standalone NEW file.
#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cortrix/query/query_request.h"

namespace cortrix {
namespace {

using json = nlohmann::json;

// A generator of VALID-UTF-8 strings. FromJsonTextMatchesInMemory serializes the
// generated body via json::dump(), which THROWS type_error.316 on a string holding
// invalid UTF-8 — so the string-valued fields must be valid UTF-8. (This does not
// weaken the wrong-type coverage: a string fed to an integer/boolean field still
// exercises that guard's false arm regardless of the string's bytes.)
rc::Gen<std::string> genUtf8String() {
    static const std::vector<std::string> kAtoms = {
        "a", "Z", "0", "_", "-", " ", "auto", "simple", "complex", "doc",
        "é" /* é */, "中" /* U+4E2D */};
    return rc::gen::map(
        rc::gen::container<std::vector<std::size_t>>(
            rc::gen::inRange<std::size_t>(0, kAtoms.size())),
        [](const std::vector<std::size_t>& idxs) {
            std::string s;
            for (std::size_t i : idxs) s += kAtoms[i];
            return s;
        });
}

// A generator of "arbitrary JSON scalar of a random type" — string / int / double /
// bool / null / empty-array / empty-object. Feeding these into a field whose guard
// expects a specific type exercises both the TRUE and (mostly) FALSE guard arms.
rc::Gen<json> genJsonScalar() {
    return rc::gen::oneOf(
        rc::gen::map(genUtf8String(), [](std::string s) { return json(s); }),
        rc::gen::map(rc::gen::arbitrary<int>(), [](int n) { return json(n); }),
        rc::gen::map(rc::gen::arbitrary<double>(), [](double d) { return json(d); }),
        rc::gen::map(rc::gen::arbitrary<bool>(), [](bool b) { return json(b); }),
        rc::gen::just(json(nullptr)),
        rc::gen::just(json::array()),
        rc::gen::just(json::object()));
}

// Build a request body object with a random subset of the known keys, each assigned
// a random-typed scalar (so types frequently MISmatch the field's expected type).
rc::Gen<json> genRequestBody() {
    return rc::gen::exec([] {
        json j = json::object();
        // query/namespace strings present often (so the body usually validates).
        if (*rc::gen::arbitrary<bool>()) j["query"] = *genUtf8String();
        if (*rc::gen::arbitrary<bool>()) j["namespace"] = *genUtf8String();
        // The optional fields get random-typed values.
        static const char* kKeys[] = {"top_k",   "timeout_ms", "explain",
                                       "route",   "granularity"};
        for (const char* k : kKeys) {
            if (*rc::gen::arbitrary<bool>()) j[k] = *genJsonScalar();
        }
        // filters / search_config as random-typed values (often not objects).
        if (*rc::gen::arbitrary<bool>()) j["filters"] = *genJsonScalar();
        if (*rc::gen::arbitrary<bool>()) j["search_config"] = *genJsonScalar();
        return j;
    });
}

// =====================================================================
// FromJson — total over any JSON object, never throws, always Ok.
// =====================================================================

// For ANY JSON object body, FromJson returns Ok and does not throw. (It is the
// lenient pre-Validate stage; rejection is Validate's job.)
RC_GTEST_PROP(QueryRequestPropR9, FromJsonTotalOverObjects, ()) {
    const json body = *genRequestBody();
    QueryRequest req;
    RC_ASSERT(QueryRequest::FromJson(body, &req).ok());
}

// The default-preservation invariant (the R8-verified core contract): starting from
// a fresh request, parsing a body in which a field is EITHER absent OR present-but-
// wrong-type must leave that field at its constructed default. We check the two
// enum-ish string fields whose default is "auto", plus the numeric defaults.
RC_GTEST_PROP(QueryRequestPropR9, WrongTypedOrAbsentKeepsDefault, ()) {
    json body = *genRequestBody();

    // Reference defaults from a pristine request.
    const QueryRequest def;

    QueryRequest req;
    RC_ASSERT(QueryRequest::FromJson(body, &req).ok());

    // route: stays "auto" unless body has a STRING route.
    if (!(body.contains("route") && body["route"].is_string())) {
        RC_ASSERT(req.route == def.route);  // "auto"
    } else {
        RC_ASSERT(req.route == body["route"].get<std::string>());
    }
    // granularity: stays "auto" unless body has a STRING granularity.
    if (!(body.contains("granularity") && body["granularity"].is_string())) {
        RC_ASSERT(req.granularity == def.granularity);  // "auto"
    } else {
        RC_ASSERT(req.granularity == body["granularity"].get<std::string>());
    }
    // top_k: stays default unless body has an INTEGER top_k.
    if (!(body.contains("top_k") && body["top_k"].is_number_integer())) {
        RC_ASSERT(req.top_k == def.top_k);
    }
    // timeout_ms: stays default unless body has an INTEGER timeout_ms.
    if (!(body.contains("timeout_ms") && body["timeout_ms"].is_number_integer())) {
        RC_ASSERT(req.timeout_ms == def.timeout_ms);
    }
    // explain: stays false unless body has a BOOLEAN explain.
    if (!(body.contains("explain") && body["explain"].is_boolean())) {
        RC_ASSERT(req.explain == def.explain);
    }
}

// search_config booleans: an enable_* flag keeps its (true) default unless the body
// provides a real boolean for it. Generated bodies put random-typed search_config
// values, so the "not an object" and "object with wrong-typed field" cases are both
// hit and the three flags must never flip on a non-boolean.
RC_GTEST_PROP(QueryRequestPropR9, SearchConfigFlagsDefaultUnlessBool, ()) {
    // Build a search_config that is sometimes an object with random-typed enable_*.
    json sc;
    const int shape = *rc::gen::inRange(0, 3);
    if (shape == 0) {
        sc = *genJsonScalar();  // often not an object
    } else {
        sc = json::object();
        for (const char* k : {"enable_vector", "enable_bm25", "enable_sql"}) {
            if (*rc::gen::arbitrary<bool>()) sc[k] = *genJsonScalar();
        }
    }
    json body = {{"query", "q"}, {"namespace", "ns"}, {"search_config", sc}};

    const QueryRequest def;
    QueryRequest req;
    RC_ASSERT(QueryRequest::FromJson(body, &req).ok());

    auto check = [&](const char* key, bool def_val, bool got) {
        bool has_bool = sc.is_object() && sc.contains(key) && sc[key].is_boolean();
        if (!has_bool) {
            RC_ASSERT(got == def_val);
        } else {
            RC_ASSERT(got == sc[key].get<bool>());
        }
    };
    check("enable_vector", def.search_config.enable_vector, req.search_config.enable_vector);
    check("enable_bm25", def.search_config.enable_bm25, req.search_config.enable_bm25);
    check("enable_sql", def.search_config.enable_sql, req.search_config.enable_sql);
}

// =====================================================================
// Normalize — truncation invariant.
// =====================================================================

// After Normalize(), the query length never exceeds kMaxQueryLength, and the
// returned/`was_truncated` flag is true IFF a trim happened — for any starting
// query length. (kMaxQueryLength is private, so we infer it from the pristine
// behaviour: build strings around whatever the limit is by probing.)
RC_GTEST_PROP(QueryRequestPropR9, NormalizeBoundsQueryLength,
              (const std::string& seed)) {
    // Construct a query of a random, possibly very large size by repeating the seed.
    const int reps = *rc::gen::inRange(0, 400);
    std::string q;
    q.reserve(seed.size() * static_cast<size_t>(reps));
    for (int i = 0; i < reps; ++i) q += seed.empty() ? std::string("x") : seed;

    QueryRequest req;
    req.query = q;
    const size_t before = req.query.size();
    bool truncated = req.Normalize();
    const size_t after = req.query.size();

    // The post-condition: length is non-increasing, and `truncated` exactly tracks
    // whether the size shrank.
    RC_ASSERT(after <= before);
    RC_ASSERT(truncated == (after < before));
    RC_ASSERT(truncated == req.was_truncated);
    // Idempotence: a second Normalize never truncates further.
    bool again = req.Normalize();
    RC_ASSERT(!again);
    RC_ASSERT(req.query.size() == after);
}

// =====================================================================
// Round-trip through a serialized JSON string (parse-from-text path).
// =====================================================================

// FromJson over the TEXT serialization of a generated body behaves identically to
// FromJson over the in-memory json (no surprise from json::parse re-typing). Guards
// the "body arrives as an HTTP string" real path.
RC_GTEST_PROP(QueryRequestPropR9, FromJsonTextMatchesInMemory, ()) {
    const json body = *genRequestBody();
    const std::string text = body.dump();

    QueryRequest a;
    RC_ASSERT(QueryRequest::FromJson(body, &a).ok());

    json reparsed = json::parse(text);
    QueryRequest b;
    RC_ASSERT(QueryRequest::FromJson(reparsed, &b).ok());

    RC_ASSERT(a.query == b.query);
    RC_ASSERT(a.namespace_name == b.namespace_name);
    RC_ASSERT(a.top_k == b.top_k);
    RC_ASSERT(a.timeout_ms == b.timeout_ms);
    RC_ASSERT(a.route == b.route);
    RC_ASSERT(a.granularity == b.granularity);
    RC_ASSERT(a.explain == b.explain);
}

}  // namespace
}  // namespace cortrix
