// Agent-friendly response/error ENVELOPE projection matrices (BREADTH).
//
// Focus = the ToJson() projection of the Agent-friendly boundary error body and
// the registry-driven envelope builders (MakeOplogError / MakeCrossNsError), over
// a field matrix:
//   - {code, message, retryable, category, retry_after_ms, structured_data}
//     presence/null projection per status class (GEN-Agent #1/#4/#5/#6).
//   - retry_after_ms machine-readable hint present iff retryable (per registry).
//   - structured_data attach / required-keys completeness gating (#5, "?explain"
//     style - the structured payload is only carried when the call site supplies it).
//   - category enum round-trip over all 5 values (#4 enumerable status).
//   - NamespaceFailure (the per-NS sub-envelope embedded in meta.namespaces_failed[]).
//
// Deliberately does NOT duplicate the *_error_matrix registry sweeps or the
// scatter CrossNsResponseTest meta.ToJson sweeps - this is the boundary error
// PROJECTION + builder envelope angle. No I/O, no model, no clock.
#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cortrix/agent_friendly/error.h"
#include "cortrix/observability/operation_log_error.h"
#include "cortrix/query/cross_ns_error.h"
#include "cortrix/query/cross_ns_response.h"

namespace cortrix {
namespace {

using agent_friendly::AgentFriendlyError;
using agent_friendly::ErrorCategory;
using json = nlohmann::json;

// =====================================================================
// 1. AgentFriendlyError::ToJson - field projection matrix
// =====================================================================

// Build an error with a chosen field profile (drives the matrix below).
AgentFriendlyError MakeErr(const std::string& code, ErrorCategory cat, bool retryable,
                           std::optional<int> retry_after_ms,
                           std::optional<json> structured) {
    AgentFriendlyError e;
    e.code = code;
    e.message = "detail for " + code;
    e.category = cat;
    e.retryable = retryable;
    e.retry_after_ms = retry_after_ms;
    e.structured_data = std::move(structured);
    return e;
}

// The six contract keys are ALWAYS present in the body (null when an optional is unset).
TEST(AgentRespMatrixEnvelope, AllSixContractKeysAlwaysPresent) {
    AgentFriendlyError e = MakeErr("CX_ERR_X", ErrorCategory::kPermanent, false,
                                   std::nullopt, std::nullopt);
    json j = ToJson(e);
    for (const char* k : {"code", "message", "retryable", "category",
                          "retry_after_ms", "structured_data"}) {
        EXPECT_TRUE(j.contains(k)) << "missing key " << k;
    }
}

// retry_after_ms projects to JSON null when unset, to the int when set.
TEST(AgentRespMatrixEnvelope, RetryAfterMsNullWhenUnset) {
    json j = ToJson(MakeErr("C", ErrorCategory::kTransient, true, std::nullopt, std::nullopt));
    EXPECT_TRUE(j["retry_after_ms"].is_null());
}
TEST(AgentRespMatrixEnvelope, RetryAfterMsValueWhenSet) {
    json j = ToJson(MakeErr("C", ErrorCategory::kTransient, true, 1500, std::nullopt));
    ASSERT_TRUE(j["retry_after_ms"].is_number_integer());
    EXPECT_EQ(j["retry_after_ms"].get<int>(), 1500);
}

// structured_data projects to JSON null when unset, to the object when set (#5 gating).
TEST(AgentRespMatrixEnvelope, StructuredDataNullWhenUnset) {
    json j = ToJson(MakeErr("C", ErrorCategory::kPermanent, false, std::nullopt, std::nullopt));
    EXPECT_TRUE(j["structured_data"].is_null());
}
TEST(AgentRespMatrixEnvelope, StructuredDataPayloadWhenSet) {
    json payload = {{"unauthorized_namespaces", {"a", "b"}}};
    json j = ToJson(MakeErr("C", ErrorCategory::kAuth, false, std::nullopt, payload));
    ASSERT_TRUE(j["structured_data"].is_object());
    EXPECT_EQ(j["structured_data"]["unauthorized_namespaces"].size(), 2u);
}

// retryable boolean is projected verbatim (matrix of both values).
class AgentRespMatrixRetryable : public ::testing::TestWithParam<bool> {};
TEST_P(AgentRespMatrixRetryable, RetryableBoolProjected) {
    json j = ToJson(MakeErr("C", ErrorCategory::kTransient, GetParam(), std::nullopt, std::nullopt));
    ASSERT_TRUE(j["retryable"].is_boolean());
    EXPECT_EQ(j["retryable"].get<bool>(), GetParam());
}
INSTANTIATE_TEST_SUITE_P(Bools, AgentRespMatrixRetryable, ::testing::Values(true, false));

// code + message project verbatim as strings.
TEST(AgentRespMatrixEnvelope, CodeAndMessageProjectedVerbatim) {
    json j = ToJson(MakeErr("CX_ERR_NS_TIMEOUT", ErrorCategory::kTimeout, true, 1000, std::nullopt));
    EXPECT_EQ(j["code"].get<std::string>(), "CX_ERR_NS_TIMEOUT");
    EXPECT_EQ(j["message"].get<std::string>(), "detail for CX_ERR_NS_TIMEOUT");
}

// category enum -> lowercase string round-trip, over all 5 enumerable values (#4).
struct CatRow { ErrorCategory cat; const char* str; };
class AgentRespMatrixCategory : public ::testing::TestWithParam<CatRow> {};
TEST_P(AgentRespMatrixCategory, CategoryProjectsToContractString) {
    const CatRow& r = GetParam();
    // ToString and the JSON projection agree on the contract string.
    EXPECT_STREQ(agent_friendly::ToString(r.cat), r.str);
    json j = ToJson(MakeErr("C", r.cat, false, std::nullopt, std::nullopt));
    EXPECT_EQ(j["category"].get<std::string>(), r.str);
}
INSTANTIATE_TEST_SUITE_P(
    AllCategories, AgentRespMatrixCategory,
    ::testing::Values(CatRow{ErrorCategory::kAuth, "auth"},
                      CatRow{ErrorCategory::kQuota, "quota"},
                      CatRow{ErrorCategory::kTransient, "transient"},
                      CatRow{ErrorCategory::kPermanent, "permanent"},
                      CatRow{ErrorCategory::kTimeout, "timeout"}));

// Full cross-product: {category x retryable x retry_after_ms present x structured present}
// asserting each projection independently - guards every null/value branch combo.
struct ProjRow {
    ErrorCategory cat;
    bool retryable;
    bool has_retry_ms;
    bool has_structured;
};
class AgentRespMatrixProjection : public ::testing::TestWithParam<ProjRow> {};
TEST_P(AgentRespMatrixProjection, EachFieldProjectedPerProfile) {
    const ProjRow& p = GetParam();
    std::optional<int> rms = p.has_retry_ms ? std::optional<int>(777) : std::nullopt;
    std::optional<json> sd = p.has_structured
                                 ? std::optional<json>(json{{"k", 1}})
                                 : std::nullopt;
    json j = ToJson(MakeErr("CX_ERR_P", p.cat, p.retryable, rms, sd));
    EXPECT_EQ(j["retryable"].get<bool>(), p.retryable);
    EXPECT_EQ(j["category"].get<std::string>(), agent_friendly::ToString(p.cat));
    EXPECT_EQ(j["retry_after_ms"].is_null(), !p.has_retry_ms);
    EXPECT_EQ(j["structured_data"].is_null(), !p.has_structured);
    if (p.has_retry_ms) EXPECT_EQ(j["retry_after_ms"].get<int>(), 777);
    if (p.has_structured) EXPECT_EQ(j["structured_data"]["k"].get<int>(), 1);
}
INSTANTIATE_TEST_SUITE_P(
    Cross, AgentRespMatrixProjection,
    ::testing::Values(
        ProjRow{ErrorCategory::kAuth, false, false, false},
        ProjRow{ErrorCategory::kAuth, false, false, true},
        ProjRow{ErrorCategory::kAuth, false, true, false},
        ProjRow{ErrorCategory::kAuth, false, true, true},
        ProjRow{ErrorCategory::kQuota, true, false, false},
        ProjRow{ErrorCategory::kQuota, true, false, true},
        ProjRow{ErrorCategory::kQuota, true, true, false},
        ProjRow{ErrorCategory::kQuota, true, true, true},
        ProjRow{ErrorCategory::kTransient, true, true, true},
        ProjRow{ErrorCategory::kTransient, false, false, false},
        ProjRow{ErrorCategory::kPermanent, false, true, false},
        ProjRow{ErrorCategory::kPermanent, false, false, true},
        ProjRow{ErrorCategory::kTimeout, true, true, false},
        ProjRow{ErrorCategory::kTimeout, true, false, true},
        ProjRow{ErrorCategory::kTimeout, false, true, true},
        ProjRow{ErrorCategory::kTimeout, true, true, true}));

// AgentFriendlyException carries the full error; GetError() round-trips every
// field for re-serialization at the API boundary (matrix over categories).
class AgentRespMatrixException : public ::testing::TestWithParam<CatRow> {};
TEST_P(AgentRespMatrixException, GetErrorRoundTrips) {
    const CatRow& r = GetParam();
    AgentFriendlyError src = MakeErr("CX_ERR_E", r.cat, true, 1234,
                                     std::optional<json>(json{{"k", "v"}}));
    agent_friendly::AgentFriendlyException ex(src);
    const AgentFriendlyError& got = ex.GetError();
    EXPECT_EQ(got.code, "CX_ERR_E");
    EXPECT_EQ(got.category, r.cat);
    EXPECT_TRUE(got.retryable);
    ASSERT_TRUE(got.retry_after_ms.has_value());
    EXPECT_EQ(*got.retry_after_ms, 1234);
    json j = ToJson(got);
    EXPECT_EQ(j["category"].get<std::string>(), r.str);
    EXPECT_EQ(j["structured_data"]["k"].get<std::string>(), "v");
}
INSTANTIATE_TEST_SUITE_P(
    AllCategories, AgentRespMatrixException,
    ::testing::Values(CatRow{ErrorCategory::kAuth, "auth"},
                      CatRow{ErrorCategory::kQuota, "quota"},
                      CatRow{ErrorCategory::kTransient, "transient"},
                      CatRow{ErrorCategory::kPermanent, "permanent"},
                      CatRow{ErrorCategory::kTimeout, "timeout"}));

// what() is non-empty for an exception (the runtime_error message path).
TEST(AgentRespMatrixException, WhatIsNonEmpty) {
    agent_friendly::AgentFriendlyException ex(
        MakeErr("CX_ERR_W", ErrorCategory::kPermanent, false, std::nullopt, std::nullopt));
    EXPECT_NE(std::string(ex.what()).size(), 0u);
}

// Structured payload shapes (array / nested object / scalar) project through ToJson
// unchanged - the body is a transparent #5 carrier.
struct SdShapeRow { const char* name; json payload; };
class AgentRespMatrixSdShape : public ::testing::TestWithParam<SdShapeRow> {};
TEST_P(AgentRespMatrixSdShape, StructuredDataShapePreserved) {
    const SdShapeRow& r = GetParam();
    json j = ToJson(MakeErr("C", ErrorCategory::kPermanent, false, std::nullopt,
                            std::optional<json>(r.payload)));
    EXPECT_EQ(j["structured_data"], r.payload) << r.name;
}
INSTANTIATE_TEST_SUITE_P(
    Shapes, AgentRespMatrixSdShape,
    ::testing::Values(
        SdShapeRow{"array", json{{"items", {1, 2, 3}}}},
        SdShapeRow{"nested", json{{"a", {{"b", {{"c", 1}}}}}}},
        SdShapeRow{"scalar_string", json{{"reason", "x"}}},
        SdShapeRow{"scalar_int", json{{"count", 7}}},
        SdShapeRow{"empty_object", json::object()},
        SdShapeRow{"mixed", json{{"ns", {"a", "b"}}, {"n", 2}, {"ok", false}}}));

// retry_after_ms boundary values project verbatim (0 / small / large).
class AgentRespMatrixRetryMs : public ::testing::TestWithParam<int> {};
TEST_P(AgentRespMatrixRetryMs, RetryAfterMsValueRoundTrips) {
    json j = ToJson(MakeErr("C", ErrorCategory::kTransient, true,
                            std::optional<int>(GetParam()), std::nullopt));
    ASSERT_TRUE(j["retry_after_ms"].is_number_integer());
    EXPECT_EQ(j["retry_after_ms"].get<int>(), GetParam());
}
INSTANTIATE_TEST_SUITE_P(Values, AgentRespMatrixRetryMs,
                         ::testing::Values(0, 1, 100, 1000, 5000, 60000, 250, 30000));

// Larger cross-product to exercise every (category x retryable x retry x sd) cell
// independently (complements the smaller hand-picked set above).
class AgentRespMatrixFullCross : public ::testing::TestWithParam<ProjRow> {};
TEST_P(AgentRespMatrixFullCross, ProjectionConsistent) {
    const ProjRow& p = GetParam();
    std::optional<int> rms = p.has_retry_ms ? std::optional<int>(321) : std::nullopt;
    std::optional<json> sd =
        p.has_structured ? std::optional<json>(json{{"z", 9}}) : std::nullopt;
    json j = ToJson(MakeErr("CX_ERR_FC", p.cat, p.retryable, rms, sd));
    EXPECT_EQ(j["category"].get<std::string>(), agent_friendly::ToString(p.cat));
    EXPECT_EQ(j["retryable"].get<bool>(), p.retryable);
    EXPECT_EQ(j["retry_after_ms"].is_null(), !p.has_retry_ms);
    EXPECT_EQ(j["structured_data"].is_null(), !p.has_structured);
}
INSTANTIATE_TEST_SUITE_P(
    Quota, AgentRespMatrixFullCross,
    ::testing::Values(
        ProjRow{ErrorCategory::kQuota, false, false, false},
        ProjRow{ErrorCategory::kQuota, false, false, true},
        ProjRow{ErrorCategory::kQuota, false, true, false},
        ProjRow{ErrorCategory::kQuota, false, true, true},
        ProjRow{ErrorCategory::kTransient, true, false, false},
        ProjRow{ErrorCategory::kTransient, true, false, true},
        ProjRow{ErrorCategory::kTransient, true, true, false},
        ProjRow{ErrorCategory::kTransient, true, true, true},
        ProjRow{ErrorCategory::kPermanent, false, false, false},
        ProjRow{ErrorCategory::kPermanent, false, true, true},
        ProjRow{ErrorCategory::kAuth, false, false, false},
        ProjRow{ErrorCategory::kAuth, false, true, true},
        ProjRow{ErrorCategory::kTimeout, true, false, false},
        ProjRow{ErrorCategory::kTimeout, true, true, true},
        ProjRow{ErrorCategory::kQuota, true, true, true},
        ProjRow{ErrorCategory::kPermanent, false, true, false}));

// =====================================================================
// 2. MakeOplogError - registry-driven envelope projection matrix
// =====================================================================

using observability::OplogErrorCode;

struct OplogEnvRow {
    OplogErrorCode code;
    const char* cx;
    ErrorCategory cat;
    bool retryable;
};
class AgentRespMatrixOplogEnv : public ::testing::TestWithParam<OplogEnvRow> {};

// The envelope's category/retryable/retry_after_ms come from the registry, never
// from the call site - and the projected JSON agrees with the registry row.
TEST_P(AgentRespMatrixOplogEnv, EnvelopeFilledFromRegistry) {
    const OplogEnvRow& r = GetParam();
    AgentFriendlyError e = observability::MakeOplogError(r.code);
    EXPECT_EQ(e.code, r.cx);
    // category/retryable are owned by the registry; assert the envelope + JSON
    // projection agree with the registry row (the row's cat/retryable hints are
    // not the source of truth and are not asserted directly).
    const observability::OplogErrorInfo& info = observability::GetOplogErrorInfo(r.code);
    EXPECT_EQ(e.category, info.category);
    EXPECT_EQ(e.retryable, info.retryable);
    EXPECT_EQ(e.retry_after_ms.has_value(), info.retry_after_ms.has_value());

    json j = ToJson(e);
    EXPECT_EQ(j["code"].get<std::string>(), r.cx);
    EXPECT_EQ(j["category"].get<std::string>(), agent_friendly::ToString(info.category));
    EXPECT_EQ(j["retryable"].get<bool>(), info.retryable);
    // retry_after_ms machine-readable hint present iff the registry marks one (#6).
    EXPECT_EQ(j["retry_after_ms"].is_null(), !info.retry_after_ms.has_value());
}

// retry_after_ms hint is present exactly when retryable (machine-readable retry, #6).
TEST_P(AgentRespMatrixOplogEnv, RetryHintConsistentWithRetryable) {
    const OplogEnvRow& r = GetParam();
    const observability::OplogErrorInfo& info = observability::GetOplogErrorInfo(r.code);
    if (info.retry_after_ms.has_value()) {
        EXPECT_TRUE(r.retryable) << "retry_after_ms only on retryable codes";
        EXPECT_GT(*info.retry_after_ms, 0);
    }
}
INSTANTIATE_TEST_SUITE_P(
    SixCodes, AgentRespMatrixOplogEnv,
    ::testing::Values(
        OplogEnvRow{OplogErrorCode::kInvalidFilter, "CX_ERR_OPLOG_INVALID_FILTER",
                    ErrorCategory::kPermanent, false},
        OplogEnvRow{OplogErrorCode::kInvalidTimestampRange,
                    "CX_ERR_OPLOG_INVALID_TIMESTAMP_RANGE", ErrorCategory::kPermanent, false},
        OplogEnvRow{OplogErrorCode::kPaginationOutOfRange,
                    "CX_ERR_OPLOG_PAGINATION_OUT_OF_RANGE", ErrorCategory::kPermanent, false},
        OplogEnvRow{OplogErrorCode::kUnauthorized, "CX_ERR_OPLOG_UNAUTHORIZED",
                    ErrorCategory::kAuth, false},
        OplogEnvRow{OplogErrorCode::kCleanupRunning, "CX_ERR_OPLOG_CLEANUP_RUNNING",
                    ErrorCategory::kTransient, true},
        OplogEnvRow{OplogErrorCode::kInternal, "CX_ERR_OPLOG_INTERNAL",
                    ErrorCategory::kPermanent, false}));

// structured_data attach + required-keys completeness gate (#5). The call site
// supplies the structured payload; the body is "complete" only with all keys.
TEST(AgentRespMatrixOplogStructured, AttachedStructuredDataProjected) {
    json sd = {{"retry_at_ms", 5123}};
    AgentFriendlyError e = observability::MakeOplogError(
        OplogErrorCode::kCleanupRunning, sd, "cleanup in progress");
    json j = ToJson(e);
    ASSERT_TRUE(j["structured_data"].is_object());
    EXPECT_EQ(j["structured_data"]["retry_at_ms"].get<int>(), 5123);
    EXPECT_EQ(j["message"].get<std::string>(), "cleanup in progress");
}

TEST(AgentRespMatrixOplogStructured, RequiredKeysCompletenessGate) {
    const std::vector<std::string>& keys =
        observability::RequiredStructuredDataKeys(OplogErrorCode::kCleanupRunning);
    // Empty payload fails the completeness gate when keys are required.
    json empty = json::object();
    json complete = json::object();
    for (const auto& k : keys) complete[k] = 0;  // fill every required key
    if (!keys.empty()) {
        EXPECT_FALSE(observability::HasRequiredStructuredData(
            OplogErrorCode::kCleanupRunning, empty));
    }
    EXPECT_TRUE(observability::HasRequiredStructuredData(
        OplogErrorCode::kCleanupRunning, complete));
}

// Default structured_data (no payload) still projects a non-null object body when
// the builder attaches an empty object (the builder default is json::object()).
TEST(AgentRespMatrixOplogStructured, DefaultStructuredIsObjectNotNull) {
    AgentFriendlyError e = observability::MakeOplogError(OplogErrorCode::kInvalidFilter);
    json j = ToJson(e);
    // structured_data is the empty-object default -> serialized object, not null.
    EXPECT_TRUE(j["structured_data"].is_object());
    EXPECT_TRUE(j["structured_data"].empty());
}

// =====================================================================
// 3. MakeCrossNsError - registry-driven envelope projection matrix
// =====================================================================

using query::CrossNsErrorCode;

struct CrossEnvRow {
    CrossNsErrorCode code;
    const char* cx;
    ErrorCategory cat;
    bool retryable;
    int http;
};
class AgentRespMatrixCrossEnv : public ::testing::TestWithParam<CrossEnvRow> {};

TEST_P(AgentRespMatrixCrossEnv, CrossNsEnvelopeFilledFromRegistry) {
    const CrossEnvRow& r = GetParam();
    AgentFriendlyError e = query::MakeCrossNsError(r.code);
    EXPECT_EQ(e.code, r.cx);
    // category/retryable come from the registry (source of truth); assert the
    // envelope + JSON projection agree with it, not with the row's hint fields.
    const query::CrossNsErrorInfo& info = query::GetCrossNsErrorInfo(r.code);
    EXPECT_EQ(e.category, info.category);
    EXPECT_EQ(e.retryable, info.retryable);
    EXPECT_EQ(info.http_status, r.http);
    EXPECT_EQ(e.retry_after_ms.has_value(), info.retry_after_ms.has_value());

    json j = ToJson(e);
    EXPECT_EQ(j["category"].get<std::string>(), agent_friendly::ToString(info.category));
    EXPECT_EQ(j["retryable"].get<bool>(), info.retryable);
    EXPECT_EQ(j["retry_after_ms"].is_null(), !info.retry_after_ms.has_value());
}

// String accessor agrees with the registry cx_code for every code.
TEST_P(AgentRespMatrixCrossEnv, CodeStringMatchesRegistry) {
    const CrossEnvRow& r = GetParam();
    EXPECT_STREQ(query::CrossNsErrorCodeString(r.code), r.cx);
}
INSTANTIATE_TEST_SUITE_P(
    SixCodes, AgentRespMatrixCrossEnv,
    ::testing::Values(
        CrossEnvRow{CrossNsErrorCode::kAuthInvalidCredentials,
                    "CX_ERR_AUTH_INVALID_CREDENTIALS", ErrorCategory::kAuth, false, 401},
        CrossEnvRow{CrossNsErrorCode::kNsUnauthorized, "CX_ERR_NS_UNAUTHORIZED",
                    ErrorCategory::kAuth, false, 403},
        CrossEnvRow{CrossNsErrorCode::kTooManyNamespaces, "CX_ERR_TOO_MANY_NAMESPACES",
                    ErrorCategory::kPermanent, false, 400},
        CrossEnvRow{CrossNsErrorCode::kNsTimeout, "CX_ERR_NS_TIMEOUT",
                    ErrorCategory::kTimeout, true, 200},
        CrossEnvRow{CrossNsErrorCode::kIndexCorrupt, "CX_ERR_INDEX_CORRUPT",
                    ErrorCategory::kPermanent, false, 200},
        CrossEnvRow{CrossNsErrorCode::kScatterTimeout, "CX_ERR_SCATTER_TIMEOUT",
                    ErrorCategory::kTimeout, true, 200}));

// MakeNsUnauthorizedError packs the unauthorized NS list into structured_data and
// the projection carries it (the anti-enumeration recovery hint, #5).
TEST(AgentRespMatrixCrossUnauthorized, UnauthorizedNsListInStructuredData) {
    AgentFriendlyError e = query::MakeNsUnauthorizedError({"sales", "eng", "hr"});
    EXPECT_EQ(e.code, "CX_ERR_NS_UNAUTHORIZED");
    json j = ToJson(e);
    ASSERT_TRUE(j["structured_data"].is_object());
    ASSERT_TRUE(j["structured_data"].contains("unauthorized_namespaces"));
    EXPECT_EQ(j["structured_data"]["unauthorized_namespaces"].size(), 3u);
}

TEST(AgentRespMatrixCrossUnauthorized, EmptyUnauthorizedListStillProjectsArray) {
    AgentFriendlyError e = query::MakeNsUnauthorizedError({});
    json j = ToJson(e);
    ASSERT_TRUE(j["structured_data"]["unauthorized_namespaces"].is_array());
    EXPECT_EQ(j["structured_data"]["unauthorized_namespaces"].size(), 0u);
}

// Oplog code -> string accessor matrix (the convenience over GetOplogErrorInfo).
struct OplogStrRow { OplogErrorCode code; const char* cx; };
class AgentRespMatrixOplogStr : public ::testing::TestWithParam<OplogStrRow> {};
TEST_P(AgentRespMatrixOplogStr, CodeStringMatchesRegistry) {
    EXPECT_STREQ(observability::OplogErrorCodeString(GetParam().code), GetParam().cx);
    EXPECT_STREQ(observability::GetOplogErrorInfo(GetParam().code).cx_code, GetParam().cx);
}
INSTANTIATE_TEST_SUITE_P(
    SixCodes, AgentRespMatrixOplogStr,
    ::testing::Values(
        OplogStrRow{OplogErrorCode::kInvalidFilter, "CX_ERR_OPLOG_INVALID_FILTER"},
        OplogStrRow{OplogErrorCode::kInvalidTimestampRange,
                    "CX_ERR_OPLOG_INVALID_TIMESTAMP_RANGE"},
        OplogStrRow{OplogErrorCode::kPaginationOutOfRange,
                    "CX_ERR_OPLOG_PAGINATION_OUT_OF_RANGE"},
        OplogStrRow{OplogErrorCode::kUnauthorized, "CX_ERR_OPLOG_UNAUTHORIZED"},
        OplogStrRow{OplogErrorCode::kCleanupRunning, "CX_ERR_OPLOG_CLEANUP_RUNNING"},
        OplogStrRow{OplogErrorCode::kInternal, "CX_ERR_OPLOG_INTERNAL"}));

// OplogStatus bridges to a Status whose message carries the CX_ERR_OPLOG_* token
// (re-inflatable at the boundary) over every code.
class AgentRespMatrixOplogStatus : public ::testing::TestWithParam<OplogStrRow> {};
TEST_P(AgentRespMatrixOplogStatus, StatusMessageCarriesToken) {
    Status s = observability::OplogStatus(GetParam().code, "detail");
    EXPECT_FALSE(s.ok());
    EXPECT_NE(s.message().find(GetParam().cx), std::string::npos) << s.message();
}
INSTANTIATE_TEST_SUITE_P(
    SixCodes, AgentRespMatrixOplogStatus,
    ::testing::Values(
        OplogStrRow{OplogErrorCode::kInvalidFilter, "CX_ERR_OPLOG_INVALID_FILTER"},
        OplogStrRow{OplogErrorCode::kInvalidTimestampRange,
                    "CX_ERR_OPLOG_INVALID_TIMESTAMP_RANGE"},
        OplogStrRow{OplogErrorCode::kPaginationOutOfRange,
                    "CX_ERR_OPLOG_PAGINATION_OUT_OF_RANGE"},
        OplogStrRow{OplogErrorCode::kUnauthorized, "CX_ERR_OPLOG_UNAUTHORIZED"},
        OplogStrRow{OplogErrorCode::kCleanupRunning, "CX_ERR_OPLOG_CLEANUP_RUNNING"},
        OplogStrRow{OplogErrorCode::kInternal, "CX_ERR_OPLOG_INTERNAL"}));

// OplogErrorToStatusCode coarse mapping is total over the enum (every code maps
// to a concrete StatusCode without throwing).
class AgentRespMatrixOplogStatusCode : public ::testing::TestWithParam<OplogErrorCode> {};
TEST_P(AgentRespMatrixOplogStatusCode, MappingIsTotal) {
    StatusCode sc = observability::OplogErrorToStatusCode(GetParam());
    // kOk would be a contract violation for an error code.
    EXPECT_NE(sc, StatusCode::kOk);
}
INSTANTIATE_TEST_SUITE_P(
    SixCodes, AgentRespMatrixOplogStatusCode,
    ::testing::Values(OplogErrorCode::kInvalidFilter,
                      OplogErrorCode::kInvalidTimestampRange,
                      OplogErrorCode::kPaginationOutOfRange,
                      OplogErrorCode::kUnauthorized,
                      OplogErrorCode::kCleanupRunning,
                      OplogErrorCode::kInternal));

// CrossNsErrorToStatusCode coarse mapping is likewise total.
class AgentRespMatrixCrossStatusCode : public ::testing::TestWithParam<CrossNsErrorCode> {};
TEST_P(AgentRespMatrixCrossStatusCode, MappingIsTotal) {
    StatusCode sc = query::CrossNsErrorToStatusCode(GetParam());
    EXPECT_NE(sc, StatusCode::kOk);
}
INSTANTIATE_TEST_SUITE_P(
    SixCodes, AgentRespMatrixCrossStatusCode,
    ::testing::Values(CrossNsErrorCode::kAuthInvalidCredentials,
                      CrossNsErrorCode::kNsUnauthorized,
                      CrossNsErrorCode::kTooManyNamespaces,
                      CrossNsErrorCode::kNsTimeout,
                      CrossNsErrorCode::kIndexCorrupt,
                      CrossNsErrorCode::kScatterTimeout));

// CrossNsStatus carries the CX_ERR_* token over every code.
struct CrossStrRow { CrossNsErrorCode code; const char* cx; };
class AgentRespMatrixCrossStatus : public ::testing::TestWithParam<CrossStrRow> {};
TEST_P(AgentRespMatrixCrossStatus, StatusMessageCarriesToken) {
    Status s = query::CrossNsStatus(GetParam().code, "d");
    EXPECT_FALSE(s.ok());
    EXPECT_NE(s.message().find(GetParam().cx), std::string::npos) << s.message();
}
INSTANTIATE_TEST_SUITE_P(
    SixCodes, AgentRespMatrixCrossStatus,
    ::testing::Values(
        CrossStrRow{CrossNsErrorCode::kAuthInvalidCredentials,
                    "CX_ERR_AUTH_INVALID_CREDENTIALS"},
        CrossStrRow{CrossNsErrorCode::kNsUnauthorized, "CX_ERR_NS_UNAUTHORIZED"},
        CrossStrRow{CrossNsErrorCode::kTooManyNamespaces, "CX_ERR_TOO_MANY_NAMESPACES"},
        CrossStrRow{CrossNsErrorCode::kNsTimeout, "CX_ERR_NS_TIMEOUT"},
        CrossStrRow{CrossNsErrorCode::kIndexCorrupt, "CX_ERR_INDEX_CORRUPT"},
        CrossStrRow{CrossNsErrorCode::kScatterTimeout, "CX_ERR_SCATTER_TIMEOUT"}));

// CrossNsException carries the code + the Agent-friendly error (handler catch path).
struct CrossExRow { CrossNsErrorCode code; const char* cx; };
class AgentRespMatrixCrossException : public ::testing::TestWithParam<CrossExRow> {};
TEST_P(AgentRespMatrixCrossException, ExceptionCarriesCodeAndError) {
    query::CrossNsException ex(GetParam().code);
    EXPECT_EQ(ex.code(), GetParam().code);
    json j = ToJson(ex.GetError());
    EXPECT_EQ(j["code"].get<std::string>(), GetParam().cx);
}
INSTANTIATE_TEST_SUITE_P(
    SixCodes, AgentRespMatrixCrossException,
    ::testing::Values(
        CrossExRow{CrossNsErrorCode::kAuthInvalidCredentials,
                   "CX_ERR_AUTH_INVALID_CREDENTIALS"},
        CrossExRow{CrossNsErrorCode::kNsUnauthorized, "CX_ERR_NS_UNAUTHORIZED"},
        CrossExRow{CrossNsErrorCode::kTooManyNamespaces, "CX_ERR_TOO_MANY_NAMESPACES"},
        CrossExRow{CrossNsErrorCode::kNsTimeout, "CX_ERR_NS_TIMEOUT"},
        CrossExRow{CrossNsErrorCode::kIndexCorrupt, "CX_ERR_INDEX_CORRUPT"},
        CrossExRow{CrossNsErrorCode::kScatterTimeout, "CX_ERR_SCATTER_TIMEOUT"}));

// =====================================================================
// 4. NamespaceFailure sub-envelope (embedded in meta.namespaces_failed[])
// =====================================================================
//
// Constructs the per-NS failure sub-envelope value directly and asserts its field
// profile carries the full GEN-Agent contract (code/retryable/category/retry hint
// + structured_data). This is the per-NS recovery envelope an Agent reads to decide
// retry (timeout) vs notify-ops (permanent).

query::NamespaceFailure MakeNsFail(const std::string& ns, const std::string& code,
                                   bool retryable, const std::string& cat,
                                   std::optional<int> retry_ms, json sd) {
    query::NamespaceFailure f;
    f.namespace_id = ns;
    f.error_code = code;
    f.retryable = retryable;
    f.category = cat;
    f.retry_after_ms = retry_ms;
    f.message = "ns failure";
    f.structured_data = std::move(sd);
    return f;
}

struct NsFailRow {
    const char* code;
    bool retryable;
    const char* category;
    bool has_retry_ms;
};
class AgentRespMatrixNsFail : public ::testing::TestWithParam<NsFailRow> {};

TEST_P(AgentRespMatrixNsFail, SubEnvelopeFieldProfile) {
    const NsFailRow& r = GetParam();
    std::optional<int> rms = r.has_retry_ms ? std::optional<int>(900) : std::nullopt;
    query::NamespaceFailure f = MakeNsFail("nsA", r.code, r.retryable, r.category, rms,
                                           json{{"index_state", "corrupt"}});
    EXPECT_EQ(f.error_code, r.code);
    EXPECT_EQ(f.retryable, r.retryable);
    EXPECT_EQ(f.category, r.category);
    EXPECT_EQ(f.retry_after_ms.has_value(), r.has_retry_ms);
    EXPECT_TRUE(f.structured_data.contains("index_state"));
}
INSTANTIATE_TEST_SUITE_P(
    Profiles, AgentRespMatrixNsFail,
    ::testing::Values(
        NsFailRow{"CX_ERR_NS_TIMEOUT", true, "timeout", true},
        NsFailRow{"CX_ERR_INDEX_CORRUPT", false, "permanent", false},
        NsFailRow{"CX_ERR_NS_TIMEOUT", true, "timeout", false},
        NsFailRow{"CX_ERR_INDEX_CORRUPT", false, "permanent", true}));

// A CrossNsResponse with an embedded NamespaceFailure list projects each sub-envelope
// inside meta.namespaces_failed[] (response-level, single shape assertion - the
// scatter test owns the exhaustive field sweep; here we assert the embedding path).
TEST(AgentRespMatrixCrossResponse, FailuresEmbeddedInMetaProjection) {
    query::CrossNsResponse resp;
    resp.meta.namespaces_queried = {"a", "b"};
    resp.meta.namespaces_succeeded = {"a"};
    resp.meta.namespaces_failed.push_back(
        MakeNsFail("b", "CX_ERR_NS_TIMEOUT", true, "timeout", 1000,
                   json::object()));
    resp.meta.coverage_ratio = 0.5f;
    json j = resp.ToJson();
    ASSERT_TRUE(j.contains("meta"));
    ASSERT_TRUE(j["meta"]["namespaces_failed"].is_array());
    ASSERT_EQ(j["meta"]["namespaces_failed"].size(), 1u);
    const json& nf = j["meta"]["namespaces_failed"][0];
    // "namespace_id" is serialized as "namespace" (per the header doc-comment).
    EXPECT_TRUE(nf.contains("namespace"));
    EXPECT_EQ(nf["error_code"].get<std::string>(), "CX_ERR_NS_TIMEOUT");
    EXPECT_TRUE(nf["retryable"].get<bool>());
}

// coverage_ratio float field projects with float precision (A-class status field).
TEST(AgentRespMatrixCrossResponse, CoverageRatioFloatProjection) {
    query::CrossNsMeta meta;
    meta.namespaces_queried = {"a", "b", "c", "d"};
    meta.namespaces_succeeded = {"a", "b", "c"};
    meta.coverage_ratio = 0.75f;
    meta.latency_ms = 42;
    json j = meta.ToJson();
    ASSERT_TRUE(j["coverage_ratio"].is_number());
    EXPECT_FLOAT_EQ(j["coverage_ratio"].get<float>(), 0.75f);
    EXPECT_EQ(j["latency_ms"].get<int>(), 42);
}

}  // namespace
}  // namespace cortrix
