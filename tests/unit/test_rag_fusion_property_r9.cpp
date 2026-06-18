// R9 robustness — property-based tests for src/query/rag_fusion_types.cpp
// (VariantStrategy (de)serialization + ValidateRagFusionConfig boundaries).
//
// test_rag_fusion.cpp covers the fixed enum/validation examples. This file is the
// property lane: the round-trip + defensive-default + boundary invariants must hold
// for ALL inputs rapidcheck generates — in particular the documented safety
// invariant that a MALFORMED NS config blob can never crash the query path
// (from_json on an unknown/non-string value defaults to kParaphrase, never throws)
// and that ValidateRagFusionConfig's accept/reject is exactly the conjunction of
// its per-field ranges.
//
// All targets are pure functions. Standalone NEW file.
#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cortrix/query/rag_fusion_types.h"

namespace cortrix::query {
namespace {

using json = nlohmann::json;

// rapidcheck generator for the VariantStrategy enum (its 3 values).
rc::Gen<VariantStrategy> genStrategy() {
    return rc::gen::element(VariantStrategy::kParaphrase,
                            VariantStrategy::kSubquery,
                            VariantStrategy::kReverse);
}

// =====================================================================
// VariantStrategy ↔ string round-trips.
// =====================================================================

// enum → string → enum is the identity for every valid enum value. Pairs
// VariantStrategyString with ParseVariantStrategy (both must agree on the 3 names).
RC_GTEST_PROP(RagFusionPropR9, StrategyEnumStringRoundTrips, ()) {
    const VariantStrategy s = *genStrategy();
    VariantStrategy back = VariantStrategy::kParaphrase;
    RC_ASSERT(ParseVariantStrategy(VariantStrategyString(s), &back));
    RC_ASSERT(back == s);
}

// ParseVariantStrategy on the THREE canonical names succeeds; on ANY other string
// it returns false AND leaves the out-param untouched (the documented contract that
// lets callers keep their own default). Generated over arbitrary strings.
RC_GTEST_PROP(RagFusionPropR9, ParseStrategyUnknownLeavesOutUntouched,
              (const std::string& raw)) {
    const bool is_canonical =
        raw == "paraphrase" || raw == "subquery" || raw == "reverse";

    // Sentinel out value distinct from what a success would write for a non-canonical
    // string; assert it is preserved on failure.
    VariantStrategy out = VariantStrategy::kReverse;
    const bool ok = ParseVariantStrategy(raw, &out);
    RC_ASSERT(ok == is_canonical);
    if (!ok) {
        RC_ASSERT(out == VariantStrategy::kReverse);  // untouched
    }
}

// ParseVariantStrategy tolerates a null out-pointer (the `if (out)` guard) — it must
// still return the right boolean and never dereference null. Generated over strings.
RC_GTEST_PROP(RagFusionPropR9, ParseStrategyNullOutSafe,
              (const std::string& raw)) {
    const bool is_canonical =
        raw == "paraphrase" || raw == "subquery" || raw == "reverse";
    RC_ASSERT(ParseVariantStrategy(raw, nullptr) == is_canonical);
}

// =====================================================================
// nlohmann (de)serialization — the defensive "never throws" safety net.
// =====================================================================

// from_json on an ARBITRARY json value (string of any content, or a non-string
// type) never throws and always yields a valid enum; an unknown/non-string value
// defaults to kParaphrase. This is the load-bearing guarantee that a malformed
// namespaces.rag_fusion_config blob cannot crash the query path.
RC_GTEST_PROP(RagFusionPropR9, StrategyFromJsonNeverThrows, ()) {
    // Mix of string values (some canonical, mostly not) and non-string json types.
    const json v = *rc::gen::oneOf(
        rc::gen::map(rc::gen::arbitrary<std::string>(), [](std::string s) { return json(s); }),
        rc::gen::just(json("paraphrase")),
        rc::gen::just(json("subquery")),
        rc::gen::just(json("reverse")),
        rc::gen::map(rc::gen::arbitrary<int>(), [](int n) { return json(n); }),
        rc::gen::just(json(nullptr)),
        rc::gen::just(json::array({1, 2})),
        rc::gen::just(json::object()));

    VariantStrategy parsed = VariantStrategy::kSubquery;  // sentinel
    // Must not throw regardless of the json shape.
    parsed = v.get<VariantStrategy>();

    if (v.is_string()) {
        const std::string str = v.get<std::string>();
        if (str == "paraphrase") RC_ASSERT(parsed == VariantStrategy::kParaphrase);
        else if (str == "subquery") RC_ASSERT(parsed == VariantStrategy::kSubquery);
        else if (str == "reverse") RC_ASSERT(parsed == VariantStrategy::kReverse);
        else RC_ASSERT(parsed == VariantStrategy::kParaphrase);  // unknown → default
    } else {
        RC_ASSERT(parsed == VariantStrategy::kParaphrase);  // non-string → default
    }
}

// to_json then from_json round-trips a strategy (the serialized form is always one
// of the 3 canonical names, which from_json maps back exactly).
RC_GTEST_PROP(RagFusionPropR9, StrategyJsonRoundTrips, ()) {
    const VariantStrategy s = *genStrategy();
    json j = s;  // to_json
    RC_ASSERT(j.get<VariantStrategy>() == s);  // from_json
}

// Whole RagFusionConfig nlohmann round-trip: serialize a config with random (in-
// range and out-of-range) field values, deserialize, and the structural fields come
// back unchanged. Exercises NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE + the strategy vector
// (de)serialization together. (variant_strategies always round-trips because every
// element serializes to a canonical name.)
RC_GTEST_PROP(RagFusionPropR9, ConfigJsonRoundTrips, ()) {
    RagFusionConfig cfg;
    cfg.enabled = *rc::gen::arbitrary<bool>();
    cfg.variant_count = *rc::gen::inRange(-5, 20);
    cfg.rrf_k = *rc::gen::inRange(-5, 200);
    cfg.timeout_ms = *rc::gen::inRange<int64_t>(-100, 60000);
    cfg.variant_strategies.clear();
    const int n = *rc::gen::inRange(0, 6);
    for (int i = 0; i < n; ++i) cfg.variant_strategies.push_back(*genStrategy());

    json j = cfg;
    RagFusionConfig back = j.get<RagFusionConfig>();
    RC_ASSERT(back.enabled == cfg.enabled);
    RC_ASSERT(back.variant_count == cfg.variant_count);
    RC_ASSERT(back.rrf_k == cfg.rrf_k);
    RC_ASSERT(back.timeout_ms == cfg.timeout_ms);
    RC_ASSERT(back.variant_strategies == cfg.variant_strategies);
}

// =====================================================================
// ValidateRagFusionConfig — accept IFF every field is in range.
// =====================================================================

// The validator's verdict equals the conjunction of the documented per-field
// predicates, for ANY config. This pins the exact accept/reject boundary (an
// off-by-one in any range check would diverge from this independent re-statement).
RC_GTEST_PROP(RagFusionPropR9, ValidateMatchesFieldRanges, ()) {
    RagFusionConfig cfg;
    cfg.enabled = *rc::gen::arbitrary<bool>();
    // Span values straddling each boundary (0 and 11 are just outside [1,10], etc.).
    cfg.variant_count = *rc::gen::inRange(-2, 14);
    cfg.rrf_k = *rc::gen::inRange(-2, 5);
    cfg.timeout_ms = *rc::gen::inRange<int64_t>(-2, 5);
    cfg.variant_strategies.clear();
    const int n = *rc::gen::inRange(0, 4);
    for (int i = 0; i < n; ++i) cfg.variant_strategies.push_back(VariantStrategy::kParaphrase);

    // Independent re-statement of the spec predicate.
    const bool expect_valid =
        (cfg.variant_count >= kVariantCountMin && cfg.variant_count <= kVariantCountMax) &&
        (cfg.rrf_k > 0) &&
        (cfg.timeout_ms > 0) &&
        !(cfg.enabled && cfg.variant_strategies.empty());

    std::string field, range;
    const bool got = ValidateRagFusionConfig(cfg, &field, &range);
    RC_ASSERT(got == expect_valid);

    // On rejection, `field` names a real config field and `range` is non-empty (so
    // the CX_ERR_RAG_FUSION_CONFIG_INVALID body is always populated).
    if (!got) {
        RC_ASSERT(!field.empty());
        RC_ASSERT(!range.empty());
        RC_ASSERT(field == "variant_count" || field == "rrf_k" ||
                  field == "timeout_ms" || field == "variant_strategies");
    }
}

// The default-constructed config is ALWAYS valid (it is the shipped OSS default; a
// regression that made the defaults out-of-range would be a real bug). Trivial but
// a useful anchor — run as a property so it shares the harness.
RC_GTEST_PROP(RagFusionPropR9, DefaultConfigIsValid, ()) {
    RagFusionConfig cfg;  // defaults
    RC_ASSERT(ValidateRagFusionConfig(cfg, nullptr, nullptr));
}

// Validation only depends on the in-range-ness of fields, never throws, and is
// deterministic — calling it twice on the same config gives the same verdict and
// the same field/range out-params.
RC_GTEST_PROP(RagFusionPropR9, ValidateIsDeterministic, ()) {
    RagFusionConfig cfg;
    cfg.enabled = *rc::gen::arbitrary<bool>();
    cfg.variant_count = *rc::gen::inRange(-5, 20);
    cfg.rrf_k = *rc::gen::inRange(-5, 100);
    cfg.timeout_ms = *rc::gen::inRange<int64_t>(-5, 10000);

    std::string f1, r1, f2, r2;
    const bool a = ValidateRagFusionConfig(cfg, &f1, &r1);
    const bool b = ValidateRagFusionConfig(cfg, &f2, &r2);
    RC_ASSERT(a == b);
    RC_ASSERT(f1 == f2);
    RC_ASSERT(r1 == r2);
}

}  // namespace
}  // namespace cortrix::query
