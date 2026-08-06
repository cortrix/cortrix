/// @file test_sec_anti_enumeration.cpp
/// @brief Security tests: the anti-enumeration invariant for every
///        namespace-path-scoped protected HTTP endpoint.
///
/// CORE INVARIANT (Cortrix's most important security property — see
/// include/cortrix/query/cross_ns_error.h ("Anti-enumeration", topic 2.6):
///
///   A namespace that does NOT exist and a namespace the caller is NOT
///   authorized for MUST be INDISTINGUISHABLE to the caller. Both must return
///   the SAME error identity `CX_ERR_NS_UNAUTHORIZED` (kNsUnauthorized), the
///   SAME HTTP status, and a byte-identical JSON error envelope (modulo the
///   per-request `request_id` / `timestamp`, which are non-load-bearing). No
///   existence leak via status code, error code, error message, envelope shape,
///   or structured_data.
///
/// This test builds a SYSTEMATIC MATRIX. For every namespace-path-scoped
/// protected endpoint (the `/api/v1/namespaces/:name/...` family + the
/// `/api/v1/namespaces/:name` detail/delete routes) it drives, with the SAME
/// caller key:
///   (1) a namespace that does not exist at all,
///   (2) a namespace that exists but belongs to ANOTHER tenant (caller is not
///       authorized for it),
///   (3) a malformed / injection-shaped namespace name,
/// and asserts the three responses are equal to each other AND carry
/// CX_ERR_NS_UNAUTHORIZED with the same status. A positive control — the caller
/// hitting its OWN existing namespace — must SUCCEED, proving the gate actually
/// discriminates on authorization rather than always-denying.
///
/// ── DESIGN MODEL ───────────────────────────────────────────────────────────
/// Two principals, two correct behaviors:
///   * A SCOPED key (explicit allow-list, e.g. {tenant_a_ns}) is NOT authorized
///     for any namespace outside its list. For every such namespace — whether it
///     does not exist, belongs to another tenant, or is malformed — the gate
///     returns the SINGLE canonical identity CX_ERR_NS_UNAUTHORIZED (403), with
///     NO namespace name echoed. Existence is never leaked. This is the
///     anti-enumeration invariant (cross-NS query issue 2.6), now enforced uniformly by
///     ApiKeyAuth::Authorize + WithAuth (api_key_auth.cpp, auth_middleware.cpp).
///   * An ALL-ACCESS key requests it EXPLICITLY via the "*" wildcard
///     (allowed_namespaces = {"*"}). It is an authorized principal, so it
///     reaches the handler and gets real responses (200 for an existing
///     namespace, 404 for one that genuinely does not exist). There is no
///     enumeration concern for a principal authorized over all namespaces.
///
/// Deny-by-default: an EMPTY allow-list now grants NO access (least privilege),
/// so a blank/misconfigured key can never silently read every namespace.
///
/// These tests assert exactly that: scoped keys get an indistinguishable
/// CX_ERR_NS_UNAUTHORIZED for every out-of-scope namespace; the "*" key is a
/// deliberate authorized principal.

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "httplib.h"
#include <nlohmann/json.hpp>

#include "cortrix/auth/api_key_auth.h"
#include "cortrix/auth/auth_context.h"
#include "cortrix/config/config.h"
#include "cortrix/namespace/namespace_manager.h"
#include "cortrix/server/http_server.h"
#include "cortrix/server/routes/document_routes.h"
#include "cortrix/spc/spc_manager.h"
#include "cortrix/upload/upload_handler.h"
#include "unit/ns_pool_test_helper.h"
#include "unit/namespace_authz_test_helper.h"  // [V6] real PermissionService authz seam

namespace cortrix {
namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

// The canonical error identity that BOTH "namespace absent" and "caller
// unauthorized" must carry, per cross_ns_error.h (kNsUnauthorized → 403).
constexpr const char* kCanonicalCode = "CX_ERR_NS_UNAUTHORIZED";
constexpr int kCanonicalStatus = 403;

// A captured HTTP response reduced to its caller-visible identity. request_id /
// timestamp are intentionally STRIPPED before comparison: they are per-request
// and may legitimately differ between two otherwise-identical envelopes. Every
// OTHER caller-visible field (status, code, message, retryable, category,
// structured_data, envelope shape) must match for the namespace to be
// indistinguishable.
struct VisibleError {
    int status = 0;
    json envelope;  // full parsed body with request_id/timestamp removed

    static VisibleError Capture(const httplib::Result& res) {
        VisibleError v;
        if (!res) return v;
        v.status = res->status;
        json body;
        try {
            body = json::parse(res->body);
        } catch (...) {
            body = json{{"__unparseable_body__", res->body}};
        }
        if (body.contains("error") && body["error"].is_object()) {
            body["error"].erase("request_id");
            body["error"].erase("timestamp");
        }
        v.envelope = std::move(body);
        return v;
    }

    std::string CodeOrEmpty() const {
        if (envelope.contains("error") && envelope["error"].is_object() &&
            envelope["error"].contains("code")) {
            return envelope["error"]["code"].get<std::string>();
        }
        return "";
    }

    std::string Dump() const {
        return "status=" + std::to_string(status) + " body=" + envelope.dump();
    }
};

// ── The matrix of namespace-path-scoped protected endpoints ──────────────────
// Each row is one real registered route (verified against
// src/server/routes/document_routes.cpp + src/server/http_server.cpp). The
// driver substitutes the namespace segment with the three negative cases and
// the positive control.
enum class Method { kGet, kPost, kDelete };

struct NsEndpoint {
    const char* label;
    Method method;
    // path template: "{ns}" is replaced by the namespace under test.
    const char* path_template;
    int required_perm;  // documents which key class is the natural caller
};

// `{ns}` is the namespace path segment. ":id" path params use a fixed dummy id —
// for the negative cases the namespace gate must fire BEFORE any document lookup,
// so the id value is irrelevant to the invariant.
const std::vector<NsEndpoint>& NsEndpoints() {
    static const std::vector<NsEndpoint> kEndpoints = {
        {"GET namespace detail", Method::kGet,
         "/api/v1/namespaces/{ns}", kPermRead},
        {"GET documents list", Method::kGet,
         "/api/v1/namespaces/{ns}/documents", kPermRead},
        {"GET document status", Method::kGet,
         "/api/v1/namespaces/{ns}/documents/dummy-doc-id/status", kPermRead},
        {"DELETE document", Method::kDelete,
         "/api/v1/namespaces/{ns}/documents/dummy-doc-id", kPermWrite},
        {"POST document upload", Method::kPost,
         "/api/v1/namespaces/{ns}/documents", kPermWrite},
        {"DELETE namespace", Method::kDelete,
         "/api/v1/namespaces/{ns}", kPermAdmin},
    };
    return kEndpoints;
}

std::string Subst(const char* tmpl, const std::string& ns) {
    std::string s = tmpl;
    const std::string token = "{ns}";
    auto pos = s.find(token);
    if (pos != std::string::npos) s.replace(pos, token.size(), ns);
    return s;
}

// ── Fixture ──────────────────────────────────────────────────────────────────
// Mirrors the two existing security harnesses:
//   * test_auth_bypass.cpp  → real CortrixHttpServer + NamespaceManager + real
//                             ApiKeyAuth (Bearer / X-API-Key), drives the
//                             /api/v1/namespaces/:name routes.
//   * test_namespace_crossing.cpp → NsPoolHarness pool seeded with two tenants,
//                             real RegisterDocumentRoutes on a live httplib::Server.
// We combine both on ONE server: CortrixHttpServer registers the namespace
// detail/delete routes; RegisterDocumentRoutes is layered onto the SAME
// underlying server() (the header documents server() as the seam "for later
// Feature route registration"). The pool harness and the NamespaceManager are
// BOTH seeded with the same two namespace names so the matrix is consistent
// across both route families.
class AntiEnumerationTest : public ::testing::Test {
protected:
    static constexpr const char* kNsA = "tenant_a_ns";   // owned by tenant A
    static constexpr const char* kNsB = "tenant_b_ns";   // owned by tenant B

    void SetUp() override {
        tmp_dir_ = fs::temp_directory_path() /
                   ("cortrix_sec_antienum_" + std::to_string(getpid()));
        fs::create_directories(tmp_dir_);

        // --- Keys (real ApiKeyAuth, hashed) ---
        // Authorization is now ownership-based (ARCHITECTURE V6): a principal may
        // reach a namespace only if its tenant OWNS it (or holds an ns_acl grant).
        // a_key (tenant-a) owns kNsA; kNsB is owned by tenant-b (seeded below), so
        // a_key is NOT authorized for it. There is no per-key wildcard anymore.
        a_key_ = "antienum-tenant-a-key";
        ApiKeyConfig a_cfg;
        a_cfg.key_hash = ApiKeyAuth::HashKey(a_key_);
        a_cfg.tenant_id = "tenant-a";
        a_cfg.permissions = kPermRead | kPermWrite | kPermAdmin;
        a_cfg.expires_at = 0;

        // c_key: a SECOND non-owning principal (tenant-c owns neither kNsA nor
        //        kNsB). Used to prove the unauthorized envelope does not depend on
        //        WHICH non-authorized key asks (no membership oracle).
        c_key_ = "antienum-tenant-c-key";
        ApiKeyConfig c_cfg;
        c_cfg.key_hash = ApiKeyAuth::HashKey(c_key_);
        c_cfg.tenant_id = "tenant-c";
        c_cfg.permissions = kPermRead | kPermWrite | kPermAdmin;
        c_cfg.expires_at = 0;

        config_.server.host = "127.0.0.1";
        config_.server.thread_count = 2;
        config_.auth.enabled = true;
        config_.ns.data_dir = (tmp_dir_ / "nsmgr").string();
        config_.ns.max_active = 10;
        config_.embedding.dimension = 128;
        config_.auth.api_keys = {a_cfg, c_cfg};

        port_ = 19500 + (getpid() % 400);
        config_.server.port = port_;

        // --- NamespaceManager (backs /api/v1/namespaces/:name GET & DELETE) ---
        ns_mgr_ = std::make_unique<NamespaceManager>(config_.ns.data_dir);
        ASSERT_TRUE(ns_mgr_->Init().ok());
        ASSERT_TRUE(ns_mgr_->Create(kNsA).ok());
        ASSERT_TRUE(ns_mgr_->Create(kNsB).ok());

        auth_.LoadKeys(config_.auth.api_keys);

        // --- Pool harness (backs the document routes) ---
        pool_dir_ = tmp_dir_ / "pool";
        harness_ = std::make_unique<test::NsPoolHarness>(pool_dir_);
        ASSERT_TRUE(harness_->Admit(kNsA).ok());
        ASSERT_TRUE(harness_->Admit(kNsB).ok());

        // [V6] Real PermissionService authz by ownership: tenant-a owns kNsA,
        // tenant-b owns kNsB. a_key (tenant-a) reaches only kNsA; kNsB and any
        // non-existent namespace are CX_ERR_NS_UNAUTHORIZED (anti-enumeration).
        authz_ = std::make_unique<cortrix::test::NamespaceAuthzHarness>(
            auth_, &harness_->ipool(), "tenant-a", std::vector<std::string>{kNsA});
        authz_->AddOwned(kNsB, "tenant-b");

        // --- UploadHandler (document upload route dependency) ---
        spc_mgr_ = std::make_unique<TestSPCManager>();
        UploadConfig up_cfg;
        up_cfg.temp_dir = (tmp_dir_ / "upload").string();
        fs::create_directories(up_cfg.temp_dir);
        upload_handler_ = std::make_unique<UploadHandler>(up_cfg, *spc_mgr_);

        // --- Server: namespace routes via CortrixHttpServer, document routes
        //     layered onto the same underlying httplib server. ---
        server_ = std::make_unique<CortrixHttpServer>(config_, auth_, *ns_mgr_);
        server_->RegisterRoutes();
        RegisterDocumentRoutes(server_->server(), *upload_handler_,
                               harness_->ipool(), auth_, /*disk_monitor=*/nullptr);

        server_thread_ = std::thread([this] { server_->Start(); });

        httplib::Client cli("127.0.0.1", port_);
        for (int i = 0; i < 50; ++i) {
            auto res = cli.Get("/api/v1/health");
            if (res) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    void TearDown() override {
        if (server_) server_->Stop();
        if (server_thread_.joinable()) server_thread_.join();
        std::error_code ec;
        fs::remove_all(tmp_dir_, ec);
    }

    // No-op SPC manager (same shape as test_namespace_crossing.cpp): the upload
    // route only needs Submit() to be callable; the namespace gate fires long
    // before any task is enqueued for the negative cases.
    class TestSPCManager : public SPCManager {
    public:
        TestSPCManager() : SPCManager() {}
        Status Submit(std::shared_ptr<SPCTask>) override { return Status::Ok(); }
        int CancelBySourcePath(const std::string&) override { return 0; }
        void Start() override {}
        void Stop() override {}
        size_t QueueSize() const override { return 0; }
        SPCStage GetTaskStage(int64_t) const override { return SPCStage::kQueued; }
    };

    httplib::Headers HeadersFor(const std::string& key) {
        return {{"Authorization", "Bearer " + key}};
    }

    // Issue one request against `path` with `method`, using `key`.
    httplib::Result Drive(Method method, const std::string& path,
                          const std::string& key) {
        httplib::Client cli("127.0.0.1", port_);
        cli.set_read_timeout(5, 0);
        auto headers = HeadersFor(key);
        switch (method) {
            case Method::kGet:
                return cli.Get(path.c_str(), headers);
            case Method::kDelete:
                return cli.Delete(path.c_str(), headers);
            case Method::kPost: {
                // Minimal multipart so the upload route reaches its namespace
                // gate; for the negative cases the gate must fire first.
                httplib::MultipartFormDataItems items = {
                    {"file", "dummy bytes", "probe.txt", "text/plain"}};
                return cli.Post(path.c_str(), headers, items);
            }
        }
        return httplib::Result{nullptr, httplib::Error::Unknown};
    }

    CortrixConfig config_;
    ApiKeyAuth auth_;
    std::unique_ptr<NamespaceManager> ns_mgr_;
    std::unique_ptr<test::NsPoolHarness> harness_;
    std::unique_ptr<TestSPCManager> spc_mgr_;
    std::unique_ptr<UploadHandler> upload_handler_;
    std::unique_ptr<CortrixHttpServer> server_;
    std::thread server_thread_;
    fs::path tmp_dir_;
    fs::path pool_dir_;
    int port_ = 0;
    std::unique_ptr<cortrix::test::NamespaceAuthzHarness> authz_;  // [V6] real authz seam
    std::string a_key_;
    std::string c_key_;
};

// The three namespace names that exercise the matrix's three negative cases.
// (2) "exists, other tenant" is tenant B's namespace, seeded above.
const std::string kNonexistentNs = "this_ns_does_not_exist_anywhere";
// A weird, clearly-out-of-scope but single-PATH-SEGMENT namespace name: it must
// reach the namespace auth gate (slashes / null bytes would instead miss the
// route and 404 at the router, which is a separate concern). Injection-character
// handling is covered by test_sec_injection_ext.cpp.
const std::string kMalformedNs = "injection-attempt-OR-1-eq-1";

// =============================================================================
// Positive control: an authorized caller hitting its OWN existing namespace
// must SUCCEED. This proves the gate discriminates on authorization and is not
// a trivial always-deny (which would make the negative cases pass vacuously).
// =============================================================================

// SEC-ENUM-POS-001: tenant A reaches its OWN namespace detail (200, with data).
TEST_F(AntiEnumerationTest, PositiveControl_OwnedNamespaceDetailSucceeds) {
    auto res = Drive(Method::kGet, std::string("/api/v1/namespaces/") + kNsA, a_key_);
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200)
        << "Authorized caller must succeed on its OWN namespace (gate must not "
           "be a blanket deny). Got: " << res->status << " body=" << res->body;
}

// SEC-ENUM-POS-002: tenant A lists documents in its OWN namespace (200).
TEST_F(AntiEnumerationTest, PositiveControl_OwnedNamespaceDocListSucceeds) {
    auto res = Drive(Method::kGet,
                     std::string("/api/v1/namespaces/") + kNsA + "/documents", a_key_);
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200)
        << "Authorized caller must list docs in its OWN namespace. Got: "
        << res->status << " body=" << res->body;
}

// =============================================================================
// THE MATRIX: for each endpoint, the three negative cases must be mutually
// indistinguishable AND carry CX_ERR_NS_UNAUTHORIZED + 403.
//
// We drive the negative cases with BOTH caller classes so the report captures
// the full leak surface:
//   * allow-listed key (a_key_)   — gate blocks all three before the handler.
//   * all-access key  (all_key_)  — gate passes; handler distinguishes
//                                   existence (THE leak).
// =============================================================================

// Helper: assert a single VisibleError is the canonical NS-unauthorized identity.
void ExpectCanonical(const VisibleError& v, const std::string& where) {
    EXPECT_EQ(v.status, kCanonicalStatus)
        << where << ": expected HTTP " << kCanonicalStatus
        << " (anti-enumeration), got " << v.Dump();
    EXPECT_EQ(v.CodeOrEmpty(), kCanonicalCode)
        << where << ": expected error code " << kCanonicalCode
        << " (NS-absent and NS-unauthorized must share one identity), got "
        << v.Dump();
}

// Helper: assert three captured responses are byte-identical (modulo request_id/
// timestamp, already stripped) — i.e. the namespace is truly indistinguishable.
void ExpectIndistinguishable(const VisibleError& nonexistent,
                             const VisibleError& other_tenant,
                             const VisibleError& malformed,
                             const std::string& endpoint) {
    EXPECT_EQ(nonexistent.status, other_tenant.status)
        << endpoint << ": STATUS LEAK — nonexistent NS (" << nonexistent.status
        << ") distinguishable from other-tenant NS (" << other_tenant.status << ")";
    EXPECT_EQ(nonexistent.status, malformed.status)
        << endpoint << ": STATUS LEAK — nonexistent NS (" << nonexistent.status
        << ") distinguishable from malformed NS (" << malformed.status << ")";
    EXPECT_EQ(nonexistent.envelope, other_tenant.envelope)
        << endpoint << ": ENVELOPE LEAK — nonexistent vs other-tenant.\n  nonexistent="
        << nonexistent.envelope.dump() << "\n  other_tenant=" << other_tenant.envelope.dump();
    EXPECT_EQ(nonexistent.envelope, malformed.envelope)
        << endpoint << ": ENVELOPE LEAK — nonexistent vs malformed.\n  nonexistent="
        << nonexistent.envelope.dump() << "\n  malformed=" << malformed.envelope.dump();
}

// SEC-ENUM-OWNER: the authorized owner reaches its OWN namespace and gets a real
// response (200), proving the scoped-key denials below are authorization-based,
// not a blanket deny. Under ownership-based authz (ARCHITECTURE V6) there is no
// per-key wildcard: a principal is authorized strictly for the namespaces its
// tenant owns / is granted, so a non-owned namespace — existent or not — is
// uniformly CX_ERR_NS_UNAUTHORIZED (the anti-enumeration property is structural,
// not a special-cased 404 vs 403 branch).
TEST_F(AntiEnumerationTest, OwnerReachesOwnNamespace) {
    auto owned = VisibleError::Capture(
        Drive(Method::kGet, std::string("/api/v1/namespaces/") + kNsA, a_key_));
    EXPECT_EQ(owned.status, 200)
        << "tenant-a must reach its OWN namespace kNsA, got " << owned.Dump();
}

// SEC-ENUM-MATRIX-SCOPEDKEY (THE anti-enumeration invariant): the least-
// privileged tenant-A key. For every namespace outside its allow-list —
// nonexistent, other-tenant, or malformed alike — the WithAuth gate fires
// BEFORE the handler and returns the single canonical CX_ERR_NS_UNAUTHORIZED
// (403) with no namespace name echoed. The three negative cases are therefore
// byte-identical: existence is not leaked.
TEST_F(AntiEnumerationTest, Matrix_ScopedKey_IndistinguishableAndUnauthorized) {
    for (const auto& ep : NsEndpoints()) {
        SCOPED_TRACE(std::string("[allow-list key] ") + ep.label + " " + ep.path_template);

        const std::string p_absent = Subst(ep.path_template, kNonexistentNs);
        const std::string p_other = Subst(ep.path_template, kNsB);
        const std::string p_malformed = Subst(ep.path_template, kMalformedNs);

        VisibleError absent = VisibleError::Capture(Drive(ep.method, p_absent, a_key_));
        VisibleError other = VisibleError::Capture(Drive(ep.method, p_other, a_key_));
        VisibleError malformed = VisibleError::Capture(Drive(ep.method, p_malformed, a_key_));

        ExpectIndistinguishable(absent, other, malformed, ep.label);
        ExpectCanonical(absent, std::string(ep.label) + " / nonexistent");
        ExpectCanonical(other, std::string(ep.label) + " / other-tenant");
        ExpectCanonical(malformed, std::string(ep.label) + " / malformed");
    }
}

// =============================================================================
// Cross-key consistency: the SAME out-of-scope namespace must look identical
// regardless of WHICH non-authorized key asks. If two differently-scoped keys
// got different statuses/envelopes for tenant B's NS, the response would be an
// oracle for "is this NS in MY allow-list" — a second-order enumeration channel.
// (The "*" all-access key is excluded here: it is an AUTHORIZED principal, not a
// non-owner — see AllAccessWildcardKey_IsAuthorizedPrincipal.)
// =============================================================================

// SEC-ENUM-XKEY: other-tenant NS must yield the SAME canonical envelope for two
// DIFFERENTLY-scoped non-authorized keys (a_key scoped to kNsA, c_key scoped to
// tenant_c_ns) — neither is authorized for tenant B's NS.
TEST_F(AntiEnumerationTest, OtherTenantNamespace_SameForAllNonAuthorizedKeys) {
    for (const auto& ep : NsEndpoints()) {
        SCOPED_TRACE(std::string("[cross-key] ") + ep.label);
        const std::string p_other = Subst(ep.path_template, kNsB);

        VisibleError via_a = VisibleError::Capture(Drive(ep.method, p_other, a_key_));
        VisibleError via_c = VisibleError::Capture(Drive(ep.method, p_other, c_key_));

        EXPECT_EQ(via_a.status, via_c.status)
            << ep.label
            << ": CROSS-KEY STATUS LEAK on an out-of-scope NS — key-a got "
            << via_a.status << ", key-c got " << via_c.status
            << ". The status must not discriminate which allow-list the caller has.";
        EXPECT_EQ(via_a.envelope, via_c.envelope)
            << ep.label << ": CROSS-KEY ENVELOPE LEAK on an out-of-scope NS.\n  key-a="
            << via_a.envelope.dump() << "\n  key-c=" << via_c.envelope.dump();
        // And the shared identity is the canonical anti-enumeration contract.
        ExpectCanonical(via_a, std::string(ep.label) + " / other-tenant / key-a");
    }
}

}  // namespace
}  // namespace cortrix
