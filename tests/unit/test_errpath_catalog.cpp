#include <gtest/gtest.h>

#include <string>

#include "httplib.h"
#include <nlohmann/json.hpp>

#include "cortrix/common/status.h"
#include "cortrix/server/http_server.h"

// Error-path coverage for the route-level namespace identity CX_ERR_NAMESPACE_NOT_FOUND.
//
// This code is surfaced by the API gateway's error writer (http_server.cpp
// WriteJsonError → ResolveError → Sdk3Map): a route handler returns a Status whose
// message carries the "CX_ERR_NAMESPACE_NOT_FOUND: ..." token, and the writer
// re-inflates the GEN-Agent {code, category, retryable} envelope. We trigger that
// real path (a not-found Status from a namespace route) and assert the JSON
// envelope `error.code` plus the registry-resolved category — which distinguishes
// the Sdk3Map row from the generic StatusCode-only fallback.
//
// NOTE on the five catalog registry codes assigned to this file
// (CX_ERR_NS_ISOLATED, CX_ERR_NS_VISIBILITY_DENIED, CX_ERR_UNIT_OWNER_REMOTE,
// CX_ERR_CONTENT_HASH_MISMATCH, CX_ERR_NS_POOL_INTERNAL): no production code path
// constructs them in V1 — they exist only as registry rows in
// src/catalog/catalog_error.cpp with no enforcement site wired. They are reported
// DEFERRED rather than covered with a hollow MakeCatalogError(code) reference. See
// the structured report accompanying this change for the per-code reasons.
namespace cortrix {
namespace {

using json = nlohmann::json;

// CX_ERR_NAMESPACE_NOT_FOUND: a namespace route returns NotFound carrying the
// token; the gateway error writer surfaces it as the envelope code, and the
// Sdk3Map row pins category=permanent / retryable=false (the namespace row).
TEST(NamespaceErrPathTest, NamespaceNotFound_SurfacesCxCodeAndCategory) {
    httplib::Response res;
    // The real flow: a not-found namespace lookup returns a Status whose message
    // is the "CX_ERR_X: detail" token convention the gateway re-inflates.
    Status not_found = Status::NotFound(
        "CX_ERR_NAMESPACE_NOT_FOUND: namespace 'does-not-exist' not found");
    WriteJsonError(res, not_found, "req-ns-404");

    EXPECT_EQ(res.status, 404);  // NotFound → HTTP 404
    auto body = json::parse(res.body);
    ASSERT_TRUE(body.contains("error"));
    // The precise CX_ERR_ identity is preserved from the token (not coarsened to a
    // generic StatusCode-derived code).
    EXPECT_EQ(body["error"]["code"], "CX_ERR_NAMESPACE_NOT_FOUND");
    // Sdk3Map row resolved the true category/retryability (permanent, non-retryable).
    EXPECT_EQ(body["error"]["category"], "permanent");
    EXPECT_EQ(body["error"]["retryable"], false);
    EXPECT_EQ(body["error"]["request_id"], "req-ns-404");
    // GEN-Agent stable schema: structured_data always present.
    EXPECT_TRUE(body["error"].contains("structured_data"));
}

// Guard against a regression where the token is dropped and the writer falls back
// to the generic StatusCode-derived code: with the token present, the resolved
// code must be the rich CX_ERR_NAMESPACE_NOT_FOUND, never a bare status code.
TEST(NamespaceErrPathTest, NamespaceNotFound_TokenPreferredOverStatusFallback) {
    httplib::Response res;
    WriteJsonError(res, Status::NotFound("CX_ERR_NAMESPACE_NOT_FOUND: gone"), "");
    auto body = json::parse(res.body);
    EXPECT_EQ(body["error"]["code"], "CX_ERR_NAMESPACE_NOT_FOUND");
    EXPECT_NE(body["error"]["code"], "NOT_FOUND");  // not the generic StatusCode fallback
}

}  // namespace
}  // namespace cortrix
