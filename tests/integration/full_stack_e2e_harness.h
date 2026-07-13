#pragma once
// Full-stack E2E harness (R7 · Derek's "core flow must run end-to-end in one real
// process" mandate). The existing "full-stack" integration tests
// (test_e2e_full_server / test_cross_feature_e2e::FullHttpE2ETest) stand up a real
// httplib server but back it with NsPoolHarness — whose pool returns a NON-searchable
// FakeIndex (MockIndexFactory) and whose SPC layer is a no-op TestSPCManager. They
// exercise the HTTP/route/auth plumbing, but NOT the production assembly: a real
// PhnswIndexFactory pool + a real SPCManager driving a real SPCPipeline through real
// PHnsw. That production wiring — the D3.5 live-only assembly seams — has never been
// E2E-covered. This harness fills that gap and is the shared foundation for the R7
// full-stack E2E suite (F13 observability / ingest pipeline / multi-NS fan-out).
//
// It mirrors the production bootstrap assembly (src/server/bootstrap.cpp §7-§8):
//   CatalogDb (global cortrix_global.db, agent_trace + operation_log migrated)
//     → DefaultINSRouter + DefaultUnitRouter (real catalog routers)
//     → PhnswIndexFactory + real WriteCoordinator factory
//     → DefaultNamespacePool (the REAL pool; ns_router.SetPool late-bind)
//     → [optional] real SPCManager(real SPCPipeline + F03/F35/F38 chain seam +
//        F40 sparse registry seam + F10 cleaning-config resolver seam)
//
// Two construction tiers (a test pays only for what it needs):
//   * BuildCore()      — catalog + routers + real pool + auth. Enough for an
//                        observability E2E (F13/F18a): agent_trace lives in the
//                        global db, interaction_log in each NS's memory.db (reached
//                        via NamespaceFacade), the cross-DB owner resolver is real.
//   * BuildIngest()    — BuildCore() + a real SPCManager (real pipeline + chains).
//                        Enough for an ingest / fan-out E2E (upload → real SPC →
//                        real PHnsw → query).
//
// The embedder is a deterministic stub OnnxEmbedder(128-dim) by default — the real
// bge-m3 path is its own E2E (test_real_model_query_e2e.cpp). The stub is still the
// real OnnxEmbedder class (deterministic vectors), so the write/index/search wiring
// is genuinely exercised; only the embedding *quality* is stubbed.
//
// The harness owns the ASSEMBLY (pool / catalog / SPC / auth / server) but does NOT
// register routes — each E2E registers exactly the route groups it exercises onto
// h_->server(), using the harness accessors for the dependencies. This keeps the
// harness decoupled from any one feature's registrar set (and its includes).
//
// Usage (compose into a fixture):
//   class MyE2E : public ::testing::Test {
//    protected:
//     void SetUp() override {
//       h_ = std::make_unique<cortrix::test::FullStackE2E>();
//       h_->BuildCore();                 // or h_->BuildIngest();
//       // register the route groups this test needs onto h_->server():
//       RegisterTracesRoutesGlobal(h_->server(), h_->global_db(), h_->pool(),
//                                  h_->global_config(), h_->auth());
//       RegisterInteractionsRoutesPerNs(h_->server(), h_->pool(), h_->auth());
//       h_->Start();                     // bind + listen thread
//     }
//     std::unique_ptr<cortrix::test::FullStackE2E> h_;
//   };
//   ... auto c = h_->Client(); c.Get("/api/v1/...", h_->Bearer(h_->admin_key())); ...

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "httplib.h"
#include <sqlite3.h>

#include "cortrix/auth/api_key_auth.h"
#include "cortrix/auth/auth_context.h"
#include "cortrix/catalog/catalog_db.h"
#include "cortrix/catalog/catalog_types.h"
#include "cortrix/catalog/default_ns_router.h"
#include "cortrix/catalog/default_unit_router.h"
#include "cortrix/common/in_memory_global_config.h"
#include "cortrix/config/config.h"
#include "cortrix/observability/operation_log_schema.h"
#include "cortrix/agent_trace/agent_trace_schema.h"
#include "cortrix/resource/f05_config.h"
#include "cortrix/resource/namespace_pool.h"
#include "cortrix/resource/namespace_facade.h"
#include "cortrix/store/iindex_factory.h"
#include "cortrix/store/write_coordinator.h"
#include "cortrix/tenant/permission_service.h"   // [V6] runtime NS-authz over the real catalog

// SPC (only needed by BuildIngest, but cheap to include).
#include <iterator>
#include "cortrix/spc/parser.h"                        // IDocumentParser / ParsedDoc (StubTxtParser)
#include "cortrix/spc/parser_factory.h"
#include "cortrix/chunker/parent_child_chunker.h"     // F34 ParentChildChunker / ChunkerConfig
#include "cortrix/spc/block_assembler.h"
#include "cortrix/spc_enricher.h"                      // F03 CreateEnricher / EnricherConfig / ISpcEnricher
#include "cortrix/spc/enricher_chain.h"                // I1 EnricherChain
#include "cortrix/spc/spc_pipeline.h"
#include "cortrix/spc/spc_manager.h"
#include "cortrix/spc/onnx_embedder.h"
#include "cortrix/retrieval/sparse_index_registry.h"   // F40 SPLADE per-NS index registry

// HTTP-facing components for the ingest + query routes (BuildIngest tier).
#include "cortrix/upload/upload_handler.h"             // UploadHandler / UploadConfig
#include "cortrix/query/intent_classifier.h"           // IntentClassifier / LlmConfig
#include "cortrix/query/rrf_fusion.h"                   // RRFFusion

namespace cortrix {
namespace test {

namespace fs = std::filesystem;

/// In-process plain-text parser for the ingest E2E (no Python subprocess). The
/// production DoclingParser shells out to a Python bridge; a full-stack ingest test
/// must not depend on that, so we inject this as the factory's primary parser. It
/// reads the uploaded file and splits it into paragraphs on blank lines — preserving
/// the test document's paragraph structure (so a verbatim-duplicated paragraph yields
/// identical F34 children, which is what the dedup probes assert on). One page only
/// (plain text is page-less); paragraphs[] is the F34 chunker input.
class StubTxtParser : public cortrix::spc::IDocumentParser {
 public:
  cortrix::spc::ParsedDoc Parse(
      const std::string& filepath,
      const cortrix::spc::ParserOptions& = cortrix::spc::ParserOptions{}) override {
    std::ifstream f(filepath, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(f)),
                     std::istreambuf_iterator<char>());

    cortrix::spc::ParsedDoc doc;
    doc.status = cortrix::spc::ParserErrorCode::kOk;
    doc.parser_name = "stub_txt";
    doc.metadata.filename = fs::path(filepath).filename().string();
    doc.metadata.mime_type = "text/plain";
    doc.metadata.doc_language = "en";
    doc.metadata.page_count = 1;

    cortrix::spc::ParsedPage page;
    page.page_num = 1;
    page.page_text = text;
    page.page_metadata.page_num = 1;
    page.page_metadata.parser_used = "stub_txt";
    page.page_metadata.page_confidence = 1.0f;
    page.page_metadata.char_count = static_cast<int>(text.size());

    // Split on blank lines ("\n\n") into paragraphs; trim trailing CR/whitespace.
    size_t start = 0;
    while (start < text.size()) {
      size_t sep = text.find("\n\n", start);
      std::string para =
          text.substr(start, sep == std::string::npos ? std::string::npos : sep - start);
      // strip surrounding whitespace
      size_t b = para.find_first_not_of(" \t\r\n");
      size_t e = para.find_last_not_of(" \t\r\n");
      if (b != std::string::npos) {
        cortrix::spc::ParsedChunk chunk;
        chunk.text = para.substr(b, e - b + 1);
        chunk.page = 1;
        chunk.type = cortrix::spc::ChunkType::TEXT;
        chunk.confidence = 1.0f;
        chunk.language = "en";
        page.paragraphs.push_back(std::move(chunk));
      }
      if (sep == std::string::npos) break;
      start = sep + 2;
    }
    doc.pages.push_back(std::move(page));
    return doc;
  }

  std::vector<std::string> SupportedFormats() const override {
    return {"txt", "text", "md"};
  }
  const char* Name() const override { return "stub_txt"; }
};

/// Production-faithful full-stack assembly for E2E tests. Owns every piece in
/// declaration order so teardown is the reverse of construction (server stops
/// first, then SPC workers, then the pool, then the catalog db).
class FullStackE2E {
 public:
  FullStackE2E() {
    root_ = fs::temp_directory_path() /
            ("cortrix_fs_e2e_" + std::to_string(::getpid()) + "_" +
             std::to_string(reinterpret_cast<uintptr_t>(this)));
    fs::create_directories(root_);
  }

  ~FullStackE2E() {
    Stop();
    // SPC workers must stop before the pool they write through is destroyed.
    if (spc_mgr_) {
      spc_mgr_->Stop();
      spc_mgr_.reset();
    }
    pool_.reset();
    // routers borrow catalog_db_.db(); destroy them before the db handle.
    ns_router_.reset();
    unit_router_.reset();
    catalog_db_.reset();
    std::error_code ec;
    fs::remove_all(root_, ec);
  }

  FullStackE2E(const FullStackE2E&) = delete;
  FullStackE2E& operator=(const FullStackE2E&) = delete;

  // ── construction tiers ────────────────────────────────────────────────────

  /// Catalog (global db, agent_trace + operation_log migrated) + real routers +
  /// real PhnswIndexFactory pool + auth keys. No SPC layer.
  void BuildCore(int embedding_dim = 128) {
    dim_ = embedding_dim;

    // Global cortrix_global.db with the F18a operation_log + F13 agent_trace
    // schemas (same providers bootstrap registers). catalog_db_->db() is the
    // global handle the observability writers + RegisterOperationsRoutes /
    // RegisterTracesRoutesGlobal use.
    catalog_db_ = std::make_unique<cortrix::catalog::CatalogDb>();
    static cortrix::observability::OperationLogSchemaProvider oplog_schema;
    static cortrix::agent_trace::AgentTraceSchemaProvider agent_trace_schema;
    ASSERT_TRUE(
        catalog_db_->Open((root_ / "cortrix_global.db").string(),
                          {&oplog_schema, &agent_trace_schema})
            .ok());

    global_config_ = std::make_shared<cortrix::InMemoryGlobalConfig>();

    // Real catalog routers over the global db (CreateNamespace does the catalog
    // INSERT + — once SetPool is wired below — the F05 AdmitCreate).
    ns_router_ = std::make_unique<cortrix::catalog::DefaultINSRouter>(
        catalog_db_->db(), /*f05_pool=*/nullptr);
    unit_router_ =
        std::make_unique<cortrix::catalog::DefaultUnitRouter>(catalog_db_->db());

    // Real F05 pool with the production PhnswIndexFactory (a real, searchable
    // P-HNSW per Unit) + a real WriteCoordinator factory.
    f05_config_.data_root = (root_ / "units").string();
    pool_ = std::make_unique<cortrix::resource::DefaultNamespacePool>(
        &index_factory_, MakeRealCoordFactory(), ns_router_.get(),
        unit_router_.get(), f05_config_);
    // Late-bind the pool into the router (breaks the pool↔router ctor cycle, as
    // bootstrap does) so CreateNamespace admits into the pool.
    ns_router_->SetPool(pool_.get());

    // Auth: three keys, MVP user_id == tenant_id.
    auth_ = std::make_unique<ApiKeyAuth>();
    auth_->LoadKeys({
        MakeKey(admin_key_, "root", kPermRead | kPermWrite | kPermAdmin),
        MakeKey(user_key_, "alice", kPermRead | kPermWrite),
        MakeKey(other_key_, "mallory", kPermRead | kPermWrite),
    });

    // [V6] Runtime namespace authorization wired DIRECTLY over the harness's own
    // real catalog db (the same handle CreateNamespace inserts into). A namespace
    // created via the real router with tenant T (CreateNamespace uses
    // "default_tenant"; CreateNamespaceOwnedBy uses an explicit owner) authorizes
    // for the key whose AuthContext.tenant_id == that owner — exactly the
    // production ownership hot path (no static allow-list).
    perm_svc_ = std::make_unique<cortrix::tenant::PermissionService>(
        catalog_db_->db());
    auth_->SetNamespaceAuthorizer(
        [this](const cortrix::AuthContext& ctx, const std::string& ns) -> bool {
          auto c = perm_svc_->BatchCheck(ctx, {ns});
          return !c.empty() && c.front().can_read;
        });
    auth_->SetNamespaceLister(
        [this](const cortrix::AuthContext& ctx) -> std::vector<std::string> {
          std::vector<std::string> active =
              pool_->GetExplainState().active_namespaces;
          if (active.empty()) return active;
          auto c = perm_svc_->BatchCheck(ctx, active);
          std::vector<std::string> out;
          for (auto& x : c)
            if (x.can_read) out.push_back(x.ns_id);
          return out;
        });

    server_ = std::make_unique<httplib::Server>();
  }

  /// BuildCore() + a real SPCManager (real SPCPipeline + F03/F35/F38 enricher
  /// chain seam + F40 sparse registry seam + F10 cleaning resolver seam). The
  /// enricher chain defaults to the NullEnricher path (no LLM configured) — a
  /// caller wanting the F03/F35/F38 fakes installs them on enricher_chain()
  /// before Start(). Workers are started by Start().
  /// @param embedding_dim  vector dimension (stub default 128; real bge-m3 = 1024).
  /// @param embedder_model_path  ONNX model path. An empty path (default) selects
  ///        the deterministic stub embedder; a real model.onnx path selects real
  ///        inference (E2E-4 semantic recall). A missing non-empty path fails.
  ///        NOTE: the F05 pool opens each Unit's P-HNSW at the snapshot/default
  ///        dim (1024); for a REAL semantic-recall test use embedding_dim=1024 so the
  ///        embedder and index dimensions agree.
  void BuildIngest(int embedding_dim = 128,
                   const std::string& embedder_model_path = "") {
    BuildCore(embedding_dim);

    // Real SPC components (mirrors bootstrap §7).
    cortrix::spc::ParserFactoryConfig pf_cfg;
    parser_factory_ =
        std::make_unique<cortrix::spc::DocumentParserFactory>(pf_cfg);
    // Inject the in-process text parser as the primary (SelectPrimary routes every
    // non-image extension here) so ingest does NOT shell out to a Python Docling
    // bridge — a full-stack ingest test must run without that dependency.
    parser_factory_->SetPrimaryParser(std::make_unique<StubTxtParser>());
    chunker_ = std::make_unique<cortrix::chunker::ParentChildChunker>(
        cortrix::chunker::ChunkerConfig{});
    assembler_ = std::make_unique<cortrix::BlockAssembler>();
    embedder_ = std::make_unique<OnnxEmbedder>(embedder_model_path, dim_);
    ASSERT_TRUE(embedder_->Init().ok());

    // Single F03 enricher (NullEnricher when no LLM is configured) — the chain
    // head aliases it, exactly as bootstrap does.
    cortrix::spc::EnricherConfig enricher_cfg;
    enricher_ = cortrix::spc::CreateEnricher(enricher_cfg);

    auto pipeline = std::make_unique<cortrix::SPCPipeline>(
        *parser_factory_, *chunker_, *embedder_, *assembler_, *enricher_);
    spc_mgr_ = std::make_unique<cortrix::SPCManager>(spc_config_, *pool_,
                                                     std::move(pipeline));

    // The F03/F35/F38 chain seam (Set* proxies forward onto the managed pipeline;
    // they MUST be installed after the pipeline moved into the manager — this is
    // the assembly-order seam the E2E is here to exercise).
    spc_mgr_->SetEnricherChain(&enricher_chain_);
    // F40 SPLADE inverted-index registry seam (one instance: write side indexes,
    // read side searches — same instance so the query serves what was written).
    sparse_index_registry_ =
        std::make_unique<cortrix::retrieval::SparseIndexRegistry>(
            (root_ / "units").string());
    spc_mgr_->SetSparseIndexRegistry(sparse_index_registry_.get());

    // HTTP-facing components for the document upload + query routes (mirrors
    // bootstrap §9-§10). UploadHandler borrows the SPCManager (enqueues parse
    // tasks); query side needs the embedder (already built) + a classifier +
    // RRF fusion. All default-configured (no real LLM): IntentClassifier with a
    // default LlmConfig classifies heuristically; RRFFusion uses k=60.
    cortrix::UploadConfig upload_cfg;
    upload_handler_ =
        std::make_unique<cortrix::UploadHandler>(upload_cfg, *spc_mgr_);
    cortrix::LlmConfig intent_llm_cfg;
    classifier_ = std::make_unique<cortrix::IntentClassifier>(intent_llm_cfg);
    fusion_ = std::make_unique<cortrix::RRFFusion>();
  }

  // ── namespace admission ────────────────────────────────────────────────────

  /// Create + admit a namespace through the REAL catalog router (catalog INSERT +
  /// F05 AdmitCreate), exactly as a connector/bootstrap path would. After this the
  /// NS resolves via the pool and its per-NS memory.db / store.db / index exist.
  Status CreateNamespace(const std::string& ns) {
    cortrix::catalog::NSMetadata meta;
    meta.namespace_id = ns;
    meta.name = ns;
    // tenant_id is NOT NULL + units.tenant_id has an FK to tenants(tenant_id). The
    // catalog schema seeds the single-tenant-OSS "default_tenant" row, so reuse it
    // (no manual tenants INSERT needed) — matching how the CE OSS build creates NSs.
    meta.tenant_id = "default_tenant";
    return ns_router_->CreateNamespace(meta);
  }

  /// Create + admit a namespace OWNED BY `owner_tenant`. The cross-NS query (F04)
  /// permission check passes immediately when namespaces.tenant_id == the caller's
  /// AuthContext.tenant_id (P09 §4.6 owner hot path; CE keys set tenant_id ==
  /// user_id). So a query-permission E2E creates the namespace owned by the SAME
  /// tenant as the API key it queries with. Ensures the tenant row exists first
  /// (units.tenant_id FK -> tenants), then delegates to the real router.
  Status CreateNamespaceOwnedBy(const std::string& ns,
                                const std::string& owner_tenant) {
    // tenants(tenant_id) FK target — seed the owner tenant if absent (idempotent).
    const std::string sql =
        "INSERT OR IGNORE INTO tenants (tenant_id, type, name, created_at) VALUES ('" +
        owner_tenant + "', 'personal', '" + owner_tenant + "', 0);";
    char* err = nullptr;
    if (sqlite3_exec(catalog_db_->db(), sql.c_str(), nullptr, nullptr, &err) !=
        SQLITE_OK) {
      std::string m = err ? err : "tenant seed failed";
      sqlite3_free(err);
      return Status::Internal(m);
    }
    cortrix::catalog::NSMetadata meta;
    meta.namespace_id = ns;
    meta.name = ns;
    meta.tenant_id = owner_tenant;
    return ns_router_->CreateNamespace(meta);
  }

  /// Acquire a per-request facade for `ns` (RAII): gives the per-NS memory.db
  /// (interaction_log / memory_sessions), store, and index. The caller checks
  /// Acquire().ok(). Used to SEED owner data (interaction_log) for the F13
  /// cross-DB owner resolver, and to READ BACK blocks after an ingest.
  std::unique_ptr<cortrix::resource::NamespaceFacade> Facade(
      const std::string& ns) {
    auto f = std::make_unique<cortrix::resource::NamespaceFacade>(*pool_, ns);
    return f;
  }

  // ── server lifecycle ───────────────────────────────────────────────────────

  /// Bind to an ephemeral loopback port + start the listener thread, then wait
  /// until the server answers. Call after registering the route groups.
  void Start() {
    port_ = server_->bind_to_any_port("127.0.0.1");
    ASSERT_GT(port_, 0);
    if (spc_mgr_) spc_mgr_->Start();
    server_thread_ = std::thread([this] { server_->listen_after_bind(); });
    // Readiness probe: poll a trivial endpoint until the listener is up.
    httplib::Client probe("127.0.0.1", port_);
    probe.set_connection_timeout(0, 200000);  // 200ms
    for (int i = 0; i < 100 && !server_->is_running(); ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    for (int i = 0; i < 100; ++i) {
      if (auto r = probe.Get("/__readiness_probe__"); r) break;  // 404 is fine
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }

  void Stop() {
    if (server_) server_->stop();
    if (server_thread_.joinable()) server_thread_.join();
  }

  // ── accessors ──────────────────────────────────────────────────────────────

  httplib::Server& server() { return *server_; }
  httplib::Client Client() { return httplib::Client("127.0.0.1", port_); }
  int port() const { return port_; }

  cortrix::resource::INamespacePool& pool() { return *pool_; }
  sqlite3* global_db() { return catalog_db_->db(); }
  std::shared_ptr<cortrix::IGlobalConfig> global_config() {
    return global_config_;
  }
  ApiKeyAuth& auth() { return *auth_; }
  cortrix::catalog::DefaultINSRouter& ns_router() { return *ns_router_; }
  cortrix::SPCManager& spc_mgr() { return *spc_mgr_; }
  cortrix::spc::EnricherChain& enricher_chain() { return enricher_chain_; }
  // HTTP-facing components (BuildIngest only).
  cortrix::UploadHandler& upload_handler() { return *upload_handler_; }
  cortrix::OnnxEmbedder& embedder() { return *embedder_; }
  cortrix::IntentClassifier& classifier() { return *classifier_; }
  cortrix::RRFFusion& fusion() { return *fusion_; }
  const fs::path& root() const { return root_; }

  // Auth helpers (plaintext keys; MVP user_id == tenant_id).
  const std::string& admin_key() const { return admin_key_; }  // user_id "root", admin
  const std::string& user_key() const { return user_key_; }    // user_id "alice"
  const std::string& other_key() const { return other_key_; }  // user_id "mallory"
  httplib::Headers Bearer(const std::string& key) {
    return {{"Authorization", "Bearer " + key}};
  }

 private:
  static ApiKeyConfig MakeKey(const std::string& plaintext,
                              const std::string& tenant, int perms) {
    ApiKeyConfig k;
    k.key_hash = ApiKeyAuth::HashKey(plaintext);
    k.tenant_id = tenant;  // -> AuthContext.user_id (MVP user_id == tenant_id)
    k.permissions = perms;
    // [V6] No static allow-list: namespace authorization is the runtime
    // PermissionService seam wired in BuildCore() over the real catalog.
    return k;
  }

  // Real WriteCoordinator factory (the pool resolves + passes the 3 stores), same
  // body as bootstrap's wc_factory.
  cortrix::resource::WriteCoordinatorFactory MakeRealCoordFactory() {
    return [](const std::string& data_dir,
              const cortrix::store::WriteCoordinatorConfig& cfg,
              cortrix::store::IVectorStore* vs, cortrix::store::IMetadataStore* ms,
              cortrix::store::IBlobStore* bs)
               -> cortrix::Result<std::unique_ptr<cortrix::store::WriteCoordinator>> {
      auto coord =
          std::make_unique<cortrix::store::WriteCoordinator>(data_dir, cfg, vs, ms, bs);
      cortrix::Status init = coord->Init();
      if (!init.ok()) return init;
      return cortrix::Result<std::unique_ptr<cortrix::store::WriteCoordinator>>(
          std::move(coord));
    };
  }

  fs::path root_;
  int dim_ = 128;
  int port_ = 0;

  // Plaintext API keys (MVP: tenant_id == user_id). admin -> user_id "root" (admin
  // perms); user -> "alice"; other -> "mallory". Used both to LoadKeys in BuildCore
  // and as the Bearer tokens tests send via the *_key() accessors.
  std::string admin_key_ = "fs-e2e-admin-key";
  std::string user_key_ = "fs-e2e-alice-key";
  std::string other_key_ = "fs-e2e-mallory-key";

  // Core (declaration order = construction order; teardown reverses it).
  std::unique_ptr<cortrix::catalog::CatalogDb> catalog_db_;
  std::shared_ptr<cortrix::InMemoryGlobalConfig> global_config_;
  std::unique_ptr<cortrix::catalog::DefaultINSRouter> ns_router_;
  std::unique_ptr<cortrix::catalog::DefaultUnitRouter> unit_router_;
  cortrix::store::PhnswIndexFactory index_factory_;
  cortrix::resource::F05Config f05_config_;
  std::unique_ptr<cortrix::resource::DefaultNamespacePool> pool_;
  std::unique_ptr<ApiKeyAuth> auth_;
  // [V6] Runtime NS-authz service over catalog_db_ (borrowed handle); the seams
  // installed on auth_ capture `this` and call through it. Declared after pool_/
  // auth_ so it tears down before the catalog db it borrows.
  std::unique_ptr<cortrix::tenant::PermissionService> perm_svc_;

  // SPC (BuildIngest only).
  cortrix::SPCConfig spc_config_;
  std::unique_ptr<cortrix::spc::DocumentParserFactory> parser_factory_;
  std::unique_ptr<cortrix::chunker::ParentChildChunker> chunker_;
  std::unique_ptr<cortrix::BlockAssembler> assembler_;
  std::unique_ptr<OnnxEmbedder> embedder_;
  std::shared_ptr<cortrix::spc::ISpcEnricher> enricher_;
  cortrix::spc::EnricherChain enricher_chain_;
  std::unique_ptr<cortrix::retrieval::SparseIndexRegistry> sparse_index_registry_;
  std::unique_ptr<cortrix::SPCManager> spc_mgr_;

  // HTTP-facing (BuildIngest only): upload handler (borrows spc_mgr_) + query deps.
  std::unique_ptr<cortrix::UploadHandler> upload_handler_;
  std::unique_ptr<cortrix::IntentClassifier> classifier_;
  std::unique_ptr<cortrix::RRFFusion> fusion_;

  // Server.
  std::unique_ptr<httplib::Server> server_;
  std::thread server_thread_;
};

}  // namespace test
}  // namespace cortrix
