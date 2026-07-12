/// @file test_sec_tenant_isolation.cpp
/// @brief Security tests (Matrix B): multi-tenant data isolation.
///
/// Asserts that one tenant can never read, query, list, or dedup-bleed into
/// another tenant's data. Three layers are exercised with REAL objects:
///
///   1. Store layer (per-NS physical separation via the F05 pool / NamespaceFacade):
///        - Tenant A writes a namespace; tenant B's per-NS store sees nothing
///          (doc_get / doc_find_by_source / doc_find_by_hash / count).
///        - Content-addressed dedup does NOT bleed across tenants: byte-identical
///          content in A and B stays logically separate (each NS store is its own
///          db; find_by_hash in B never returns A's doc).
///
///   2. Authorization layer (F04 AuthorizeNamespaces, anti-enumeration):
///        - Tenant B requesting tenant A's NS → CX_ERR_NS_UNAUTHORIZED, and a
///          NON-EXISTENT NS yields the SAME error (existence never leaked).
///        - A scatter/cross-NS request only ever queries the principal's own
///          authorized set; a non-owned NS is stripped before any gather.
///
///   3. HTTP layer (real server, two API keys each scoped to its own namespace):
///        - Query endpoint: tenant B's key querying tenant A's NS → 403.
///        - Document upload endpoint: tenant B's key writing tenant A's NS.
///          (RegisterDocumentRoutes does NOT call can_access_namespace today — this
///          test asserts the SECURE behavior, so it FAILS if the gap is real. See
///          the report: this is a reported finding, not weakened to pass.)
///
/// SECURITY assertions: each test asserts the SECURE outcome. A real leak makes the
/// test FAIL — that is intended.

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "httplib.h"
#include <nlohmann/json.hpp>

#include "cortrix/auth/api_key_auth.h"
#include "cortrix/auth/auth_context.h"
#include "cortrix/common/block_header.h"
#include "cortrix/common/block_types.h"
#include "cortrix/config/config.h"
#include "cortrix/query/authorize_namespaces.h"
#include "cortrix/query/cross_ns_error.h"
#include "cortrix/query/intent_classifier.h"
#include "cortrix/query/query_routes.h"
#include "cortrix/query/rrf_fusion.h"
#include "cortrix/resource/namespace_facade.h"
#include "cortrix/server/routes/document_routes.h"
#include "cortrix/spc/onnx_embedder.h"
#include "cortrix/spc/spc_manager.h"
#include "cortrix/store/cortrix_store_sqlite.h"
#include "cortrix/upload/upload_handler.h"
#include "unit/namespace_authz_test_helper.h"  // [V6] real PermissionService authz seam
#include "scatter/mock_permission_service.h"
#include "unit/ns_pool_test_helper.h"

namespace cortrix {
namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

// ============================================================
// 1. Store-level physical isolation + dedup non-bleed
// ============================================================

class TenantStoreIsolationTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_dir_ = fs::temp_directory_path() /
                    ("cortrix_sec_tenant_store_" + std::to_string(getpid()));
        fs::create_directories(test_dir_);

        config_.ns.data_dir = test_dir_.string();
        config_.ns.max_active = 10;
        config_.embedding.dimension = 128;

        harness_ = std::make_unique<test::NsPoolHarness>(test_dir_);
        ASSERT_TRUE(harness_->Admit("tenant_a_ns").ok());
        ASSERT_TRUE(harness_->Admit("tenant_b_ns").ok());
    }

    void TearDown() override {
        harness_.reset();
        std::error_code ec;
        fs::remove_all(test_dir_, ec);
    }

    CortrixStoreSqlite* AcquireStore(const std::string& ns,
                                     std::unique_ptr<resource::NamespaceFacade>& holder) {
        holder = std::make_unique<resource::NamespaceFacade>(harness_->ipool(), ns);
        if (!holder->Acquire().ok()) return nullptr;
        return dynamic_cast<CortrixStoreSqlite*>(&holder->store());
    }

    CortrixConfig config_;
    fs::path test_dir_;
    std::unique_ptr<test::NsPoolHarness> harness_;
};

// SEC-ISO-001: Tenant A's doc is invisible to tenant B by id, source, hash, count.
TEST_F(TenantStoreIsolationTest, TenantBCannotReadTenantADoc) {
    std::unique_ptr<resource::NamespaceFacade> fa, fb;
    CortrixStoreSqlite* store_a = AcquireStore("tenant_a_ns", fa);
    ASSERT_NE(store_a, nullptr);

    CortrixDoc doc;
    doc.source_type = "confidential";
    doc.source_path = "tenant_a_secret.txt";
    doc.content_hash = "sha256:tenant_a_unique_hash_value_00";
    ASSERT_EQ(store_a->doc_create(doc), 0);
    const std::string a_doc_id = doc.doc_id;

    CortrixStoreSqlite* store_b = AcquireStore("tenant_b_ns", fb);
    ASSERT_NE(store_b, nullptr);

    CortrixDoc out;
    EXPECT_EQ(store_b->doc_get(a_doc_id, out), -2)
        << "tenant B read tenant A's doc by id";
    EXPECT_EQ(store_b->doc_find_by_source("confidential", "tenant_a_secret.txt", out), -2)
        << "tenant B read tenant A's doc by source";
    EXPECT_EQ(store_b->doc_find_by_hash("sha256:tenant_a_unique_hash_value_00", out), -2)
        << "tenant B read tenant A's doc by content_hash";

    int64_t b_count = 0;
    store_b->doc_count(&b_count);
    EXPECT_EQ(b_count, 0) << "tenant B's namespace must be empty";
}

// SEC-ISO-002: dedup non-bleed — byte-identical content in A and B stays separate.
// Each NS store is content-addressed independently; A's hash row must not satisfy
// B's find_by_hash (dedup_scope = tenant / per-NS, not global).
TEST_F(TenantStoreIsolationTest, IdenticalContentDoesNotDedupAcrossTenants) {
    const std::string shared_hash = "sha256:0011223344556677008899aabbccddee";

    std::unique_ptr<resource::NamespaceFacade> fa, fb;
    CortrixStoreSqlite* store_a = AcquireStore("tenant_a_ns", fa);
    CortrixStoreSqlite* store_b = AcquireStore("tenant_b_ns", fb);
    ASSERT_NE(store_a, nullptr);
    ASSERT_NE(store_b, nullptr);

    // Same content => same content_hash, stored independently in each tenant's NS.
    CortrixDoc da;
    da.source_type = "test";
    da.source_path = "same.txt";
    da.content_hash = shared_hash;
    ASSERT_EQ(store_a->doc_create(da), 0);

    CortrixDoc db;
    db.source_type = "test";
    db.source_path = "same.txt";
    db.content_hash = shared_hash;
    ASSERT_EQ(store_b->doc_create(db), 0);

    // Each tenant finds ONLY its own doc for that hash; the doc_ids differ.
    CortrixDoc got_a, got_b;
    ASSERT_EQ(store_a->doc_find_by_hash(shared_hash, got_a), 0);
    ASSERT_EQ(store_b->doc_find_by_hash(shared_hash, got_b), 0);
    EXPECT_EQ(got_a.doc_id, da.doc_id);
    EXPECT_EQ(got_b.doc_id, db.doc_id);
    EXPECT_NE(got_a.doc_id, got_b.doc_id)
        << "identical content across tenants must remain logically distinct rows";

    // Each NS has exactly one doc (no cross-tenant collapse).
    int64_t ca = 0, cb = 0;
    store_a->doc_count(&ca);
    store_b->doc_count(&cb);
    EXPECT_EQ(ca, 1);
    EXPECT_EQ(cb, 1);
}

// SEC-ISO-003: FTS search in tenant B never surfaces tenant A's indexed content.
TEST_F(TenantStoreIsolationTest, TenantBSearchNeverSurfacesTenantAContent) {
    std::unique_ptr<resource::NamespaceFacade> fa, fb;
    CortrixStoreSqlite* store_a = AcquireStore("tenant_a_ns", fa);
    ASSERT_NE(store_a, nullptr);

    CortrixDoc doc;
    doc.source_type = "test";
    doc.source_path = "a.txt";
    store_a->doc_create(doc);
    auto blob = BlockBuild(kBlockFile, kLevelL2, "topsecret_marker_xyzzy", "", "", 0, 0);
    CortrixBlock block;
    block.doc_id = doc.doc_id;
    block.chunk_index = 0;
    block.block_type = 1;
    block.processing_level = 2;
    block.data = blob;
    block.content_text = "topsecret_marker_xyzzy";
    store_a->block_insert(block);

    CortrixStoreSqlite* store_b = AcquireStore("tenant_b_ns", fb);
    ASSERT_NE(store_b, nullptr);
    std::vector<SearchResult> results;
    EXPECT_EQ(store_b->search_fulltext("topsecret_marker_xyzzy", 10, results), 0);
    EXPECT_TRUE(results.empty())
        << "tenant B FTS leaked tenant A content (" << results.size() << " rows)";
}

// ============================================================
// 2. Authorization-layer isolation (F04 AuthorizeNamespaces)
// ============================================================

namespace q = cortrix::query;

// AuthContext is cortrix::AuthContext (auth_context.h), not in the query namespace.
cortrix::AuthContext MakeAuth(const std::string& user_id, const std::string& tenant) {
    cortrix::AuthContext a;
    a.user_id = user_id;
    a.tenant_id = tenant;
    return a;
}

// SEC-ISO-004: tenant B (authorized only for its own NS) cannot query tenant A's
// NS — CX_ERR_NS_UNAUTHORIZED, with A's NS in the structured unauthorized list.
TEST(TenantAuthIsolationTest, TenantBDeniedTenantANamespace) {
    // Principal B may only QUERY "tenant_b_ns".
    q::MockPermissionService perm({"tenant_b_ns"});
    try {
        q::AuthorizeNamespaces({"tenant_a_ns"}, MakeAuth("bob", "tenant_b"), &perm);
        FAIL() << "expected CrossNsException (cross-tenant query must be denied)";
    } catch (const q::CrossNsException& e) {
        EXPECT_EQ(e.code(), q::CrossNsErrorCode::kNsUnauthorized);
        ASSERT_TRUE(e.GetError().structured_data.has_value());
        auto list = (*e.GetError().structured_data)["unauthorized_namespaces"];
        ASSERT_TRUE(list.is_array());
        EXPECT_EQ(list.size(), 1u);
        EXPECT_EQ(list[0], "tenant_a_ns");
    }
}

// SEC-ISO-005: anti-enumeration — a NON-EXISTENT NS yields the SAME unauthorized
// error as an existing-but-foreign NS, so tenant B cannot probe which of tenant
// A's namespaces exist.
TEST(TenantAuthIsolationTest, AntiEnumerationForeignAndGhostIndistinguishable) {
    q::MockPermissionService perm({"tenant_b_ns"});

    q::CrossNsErrorCode foreign_code{}, ghost_code{};
    try {
        q::AuthorizeNamespaces({"tenant_a_ns"}, MakeAuth("bob", "tenant_b"), &perm);
        FAIL();
    } catch (const q::CrossNsException& e) {
        foreign_code = e.code();
    }
    try {
        q::AuthorizeNamespaces({"does_not_exist_ns"}, MakeAuth("bob", "tenant_b"), &perm);
        FAIL();
    } catch (const q::CrossNsException& e) {
        ghost_code = e.code();
    }
    EXPECT_EQ(foreign_code, q::CrossNsErrorCode::kNsUnauthorized);
    EXPECT_EQ(ghost_code, q::CrossNsErrorCode::kNsUnauthorized)
        << "existence of a foreign namespace must not be distinguishable";
}

// SEC-ISO-006: a cross-NS / wildcard request expands ONLY to the principal's own
// authorized set — a non-owned NS never enters the queried (gather) list, so
// scatter-gather can never fan out to another tenant's data.
TEST(TenantAuthIsolationTest, WildcardNeverExpandsBeyondOwnedSet) {
    // Principal A owns only its two namespaces; tenant B's NS exists elsewhere but
    // is NOT in A's authorized set.
    q::MockPermissionService perm({"tenant_a_ns", "tenant_a_ns2"});
    auto queried = q::AuthorizeNamespaces({"*"}, MakeAuth("alice", "tenant_a"), &perm);

    EXPECT_EQ(queried, (std::vector<std::string>{"tenant_a_ns", "tenant_a_ns2"}));
    for (const auto& ns : queried) {
        EXPECT_NE(ns, "tenant_b_ns")
            << "wildcard expansion leaked a foreign tenant's namespace into scatter";
    }
}

// SEC-ISO-007: a mixed request (one owned + one foreign) is rejected wholesale —
// the partial-authorize path does not silently query the owned subset and leak the
// foreign rejection only; the whole request aborts with the foreign NS flagged.
TEST(TenantAuthIsolationTest, MixedOwnedAndForeignRejectsWholeRequest) {
    q::MockPermissionService perm({"tenant_a_ns"});
    try {
        q::AuthorizeNamespaces({"tenant_a_ns", "tenant_b_ns"},
                               MakeAuth("alice", "tenant_a"), &perm);
        FAIL() << "a request touching a foreign NS must not be partially authorized";
    } catch (const q::CrossNsException& e) {
        EXPECT_EQ(e.code(), q::CrossNsErrorCode::kNsUnauthorized);
        auto list = (*e.GetError().structured_data)["unauthorized_namespaces"];
        ASSERT_EQ(list.size(), 1u);
        EXPECT_EQ(list[0], "tenant_b_ns");
    }
}

// ============================================================
// 3. HTTP-layer isolation (two keys, each scoped to its own NS)
// ============================================================

class TenantHttpIsolationTest : public ::testing::Test {
protected:
    void SetUp() override {
        tmp_dir_ = fs::temp_directory_path() /
                   ("cortrix_sec_tenant_http_" + std::to_string(getpid()));
        fs::create_directories(tmp_dir_);

        config_.ns.data_dir = tmp_dir_.string();
        config_.ns.max_active = 10;
        config_.embedding.dimension = 128;
        config_.upload.max_file_size = 100 * 1024 * 1024;
        config_.upload.large_file_threshold = 100 * 1024 * 1024;

        harness_ = std::make_unique<test::NsPoolHarness>(tmp_dir_);
        ASSERT_TRUE(harness_->Admit("tenant_a_ns").ok());
        ASSERT_TRUE(harness_->Admit("tenant_b_ns").ok());

        auth_ = std::make_unique<ApiKeyAuth>();
        key_a_ = "sec-tenant-a-key";
        key_b_ = "sec-tenant-b-key";
        ApiKeyConfig ka;
        ka.key_hash = ApiKeyAuth::HashKey(key_a_);
        ka.tenant_id = "tenant_a";
        ka.permissions = kPermRead | kPermWrite;
        ApiKeyConfig kb;
        kb.key_hash = ApiKeyAuth::HashKey(key_b_);
        kb.tenant_id = "tenant_b";
        kb.permissions = kPermRead | kPermWrite;
        auth_->LoadKeys({ka, kb});
        // [V6] Real PermissionService authorization by ownership: tenant_a owns
        // tenant_a_ns, tenant_b owns tenant_b_ns -> each key reaches only its own
        // namespace; the other tenant's NS is CX_ERR_NS_UNAUTHORIZED (isolation).
        authz_ = std::make_unique<cortrix::test::NamespaceAuthzHarness>(
            *auth_, &harness_->ipool(), "tenant_a",
            std::vector<std::string>{"tenant_a_ns"});
        authz_->AddOwned("tenant_b_ns", "tenant_b");

        embedder_ = std::make_unique<OnnxEmbedder>("", 128);
        embedder_->Init();
        LlmConfig llm_cfg;
        classifier_ = std::make_unique<IntentClassifier>(llm_cfg);
        fusion_ = std::make_unique<RRFFusion>();
        spc_mgr_ = std::make_unique<TestSPCManager>();
        handler_ = std::make_unique<UploadHandler>(config_.upload, *spc_mgr_);

        svr_ = std::make_unique<httplib::Server>();
        RegisterDocumentRoutes(*svr_, *handler_, harness_->ipool(), *auth_);
        RegisterQueryRoutes(*svr_, *auth_, harness_->ipool(), *embedder_,
                            *classifier_, *fusion_);

        port_ = 19700 + (getpid() % 250);
        svr_thread_ = std::thread([this] { svr_->listen("127.0.0.1", port_); });

        httplib::Client cli("127.0.0.1", port_);
        for (int i = 0; i < 50; ++i) {
            auto res = cli.Get("/api/v1/namespaces/tenant_a_ns/documents", HdrA());
            if (res) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    void TearDown() override {
        if (svr_) svr_->stop();
        if (svr_thread_.joinable()) svr_thread_.join();
        std::error_code ec;
        fs::remove_all(tmp_dir_, ec);
    }

    httplib::Headers HdrA() { return {{"Authorization", "Bearer " + key_a_}}; }
    httplib::Headers HdrB() { return {{"Authorization", "Bearer " + key_b_}}; }

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

    CortrixConfig config_;
    fs::path tmp_dir_;
    std::unique_ptr<test::NsPoolHarness> harness_;
    std::unique_ptr<ApiKeyAuth> auth_;
    std::unique_ptr<cortrix::test::NamespaceAuthzHarness> authz_;  // [V6] real authz seam
    std::unique_ptr<OnnxEmbedder> embedder_;
    std::unique_ptr<IntentClassifier> classifier_;
    std::unique_ptr<RRFFusion> fusion_;
    std::unique_ptr<TestSPCManager> spc_mgr_;
    std::unique_ptr<UploadHandler> handler_;
    std::unique_ptr<httplib::Server> svr_;
    std::thread svr_thread_;
    std::string key_a_, key_b_;
    int port_;
};

// SEC-ISO-008: tenant B's key querying tenant A's namespace must be denied (403).
// The query route enforces AuthContext::can_access_namespace against the key's
// allowed_namespaces.
TEST_F(TenantHttpIsolationTest, QueryEndpointDeniesCrossTenantNamespace) {
    httplib::Client cli("127.0.0.1", port_);

    json body;
    body["query"] = "anything";
    body["namespace"] = "tenant_a_ns";  // B's key, A's namespace
    body["top_k"] = 5;

    auto res = cli.Post("/api/v1/query", HdrB(), body.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 403)
        << "tenant B's key must be denied querying tenant A's namespace (got "
        << res->status << ")";

    // Control: B querying its OWN namespace is accepted (200/503, never 403).
    json own;
    own["query"] = "anything";
    own["namespace"] = "tenant_b_ns";
    own["top_k"] = 5;
    auto ok = cli.Post("/api/v1/query", HdrB(), own.dump(), "application/json");
    ASSERT_TRUE(ok);
    EXPECT_NE(ok->status, 403)
        << "tenant B must be able to query its own namespace";
}

// SEC-ISO-009: tenant B's key UPLOADING into tenant A's namespace must be denied.
//
// 🚨 SECURITY ASSERTION (reported finding): RegisterDocumentRoutes today does NOT
// call can_access_namespace before acquiring the namespace facade — it derives the
// namespace solely from the URL path. This test asserts the SECURE behavior (the
// write is rejected with 401/403, NOT accepted). If the gap is real, this test
// FAILS — which is the intent; it must NOT be weakened to pass a cross-tenant write.
TEST_F(TenantHttpIsolationTest, DocumentUploadDeniesCrossTenantNamespace) {
    httplib::Client cli("127.0.0.1", port_);

    httplib::MultipartFormDataItems items = {
        {"file", "tenant B writing into tenant A's namespace", "evil.txt", "text/plain"},
    };
    // B's key, A's namespace in the URL path.
    auto res = cli.Post("/api/v1/namespaces/tenant_a_ns/documents", HdrB(), items);
    ASSERT_TRUE(res);
    EXPECT_TRUE(res->status == 401 || res->status == 403)
        << "tenant B uploaded into tenant A's namespace (status " << res->status
        << ") — cross-tenant write must be denied with 401/403";

    // Regardless of the route's HTTP status, the write must NOT have landed in A's
    // store. Verify directly via tenant A's per-NS store.
    std::unique_ptr<resource::NamespaceFacade> fa =
        std::make_unique<resource::NamespaceFacade>(harness_->ipool(), "tenant_a_ns");
    ASSERT_TRUE(fa->Acquire().ok());
    auto* store_a = dynamic_cast<CortrixStoreSqlite*>(&fa->store());
    ASSERT_NE(store_a, nullptr);
    int64_t a_count = 0;
    store_a->doc_count(&a_count);
    EXPECT_EQ(a_count, 0)
        << "a cross-tenant upload materialized a document in tenant A's namespace";
}

// SEC-ISO-010: tenant B listing tenant A's documents must be denied (no metadata
// enumeration of another tenant's namespace).
//
// 🚨 SECURITY ASSERTION (reported finding): the GET documents route also derives
// the namespace from the URL path without can_access_namespace. Asserts SECURE
// behavior; FAILS if cross-tenant listing is allowed.
TEST_F(TenantHttpIsolationTest, DocumentListDeniesCrossTenantNamespace) {
    httplib::Client cli("127.0.0.1", port_);

    // Seed a doc in tenant A's namespace via A's own (authorized) key first.
    {
        httplib::MultipartFormDataItems items = {
            {"file", "tenant A private doc", "a_private.txt", "text/plain"},
        };
        auto seed = cli.Post("/api/v1/namespaces/tenant_a_ns/documents", HdrA(), items);
        ASSERT_TRUE(seed);
    }

    // Now tenant B tries to list tenant A's documents.
    auto res = cli.Get("/api/v1/namespaces/tenant_a_ns/documents", HdrB());
    ASSERT_TRUE(res);
    EXPECT_TRUE(res->status == 401 || res->status == 403)
        << "tenant B listed tenant A's documents (status " << res->status
        << ") — cross-tenant metadata enumeration must be denied";

    // If the route returned a body, it must NOT disclose A's private filename.
    if (!res->body.empty()) {
        EXPECT_EQ(res->body.find("a_private.txt"), std::string::npos)
            << "tenant A's document metadata leaked to tenant B";
    }
}

}  // namespace
}  // namespace cortrix
