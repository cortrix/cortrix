#pragma once
#include <memory>
#include <string>

#include "httplib.h"

#include "cortrix/auth/api_key_auth.h"
#include "cortrix/common/status.h"
#include "cortrix/query/rrf_fusion.h"
#include "cortrix/resource/namespace_pool.h"

namespace cortrix {
class OnnxEmbedder;
namespace tenant {
class PermissionService;
}  // namespace tenant
namespace llm {
class ILlmClient;
}  // namespace llm
namespace retrieval {
class SparseIndexRegistry;
}  // namespace retrieval
namespace agent_trace {
class EngineInstrumentation;
}  // namespace agent_trace
}  // namespace cortrix

namespace cortrix::query {

/// Validate the bounded parent-expansion request controls (RETRIEVAL_TYPES_SPEC
/// §2 v1.2). Exposed here (rather than living in the route's translation unit)
/// because the RULE is the part worth pinning in a unit test; reading the values
/// off the query string / JSON body is plumbing.
///
/// `include_parent` defaults to false, so an absent control leaves parent-text
/// expansion off. It does NOT leave the response byte-identical to a pre-F34 server:
/// `parent_id` is populated on every result either way. What the switch gates is the
/// parent TEXT and its span metadata. When it is true, `expand_top_k >= 1` is REQUIRED: the
/// bound is a measurement outcome, not a design constant, so the API refuses to
/// invent one rather than silently expanding the whole result set (a full top_k of
/// ~1024-token parents is roughly 80 KB, past the agent-side single-response
/// budget, where the host truncates and the answer ends up less complete).
/// `max_bytes` is optional (0 = no extra byte cap). Negative values are rejected.
Status ValidateParentExpansion(bool include_parent, int expand_top_k, int max_bytes);

/// CrossNsQueryWiring — the D3.5 assembly that mounts the F04 cross-NS query stack
/// (LiveNamespacePipeline → SingleUnitExecutor → ScatterGather → CrossNsQueryHandler)
/// as the live POST /api/v1/query handler, replacing the MVP single-NS route.
///
/// It owns the long-lived F04 components (the shared ExecutorEngine, the reranker,
/// the live pipeline + executor, the ScatterGather, the query→tenant permission
/// adapter, the handler) so they outlive the registered httplib closure. Construct
/// once at bootstrap, keep it alive for the server's lifetime, and call Register().
///
/// In CE no-auth mode the AuthMiddleware leaves
/// AuthContext.user_id empty; Register()'s closure injects a single-tenant
/// AuthContext{user_id="default"} so F04's AuthorizeNamespaces Step 1 passes
/// (F04 §4.2 v1.0.4 — the 401 only fires when auth is enabled and unauthenticated).
class CrossNsQueryWiring {
public:
    /// @param pool      the F05 namespace pool (NOT owned; outlives this).
    /// @param embedder  the shared F03 OnnxEmbedder (NOT owned).
    /// @param fusion    the shared RRFFusion (NOT owned).
    /// @param perm_svc  the P09 tenant PermissionService over catalog.db (NOT owned).
    /// @param sparse_registry  the shared F40 per-NS SPLADE index owner (NOT owned;
    ///                 bootstrap owns it and also hands it to the SPC write path, so
    ///                 the read path serves exactly what ingest indexed). null →
    ///                 sparse route off (read degrades to dense+FTS5).
    /// @param llm       shared LLM client for F36 query-variant expansion (NOT
    ///                 owned via raw ptr — shared_ptr). null → F36 stays off (it
    ///                 needs an LLM to expand; default-disabled either way).
    /// @param engine_instr  F13 Engine-layer instrumentation (§11, S6). When set,
    ///                 the query closure records one agent_trace row per executed
    ///                 query (success + failure), reading trace/session/agent/user
    ///                 from the thread-local ObservabilityContext WithAuth filled.
    ///                 null → tracing off (standalone / tests), behavior otherwise
    ///                 unchanged.
    CrossNsQueryWiring(cortrix::resource::INamespacePool& pool,
                       cortrix::OnnxEmbedder& embedder,
                       RRFFusion& fusion,
                       cortrix::tenant::PermissionService& perm_svc,
                       cortrix::retrieval::SparseIndexRegistry* sparse_registry = nullptr,
                       std::shared_ptr<cortrix::llm::ILlmClient> llm = nullptr,
                       std::shared_ptr<agent_trace::EngineInstrumentation> engine_instr =
                           nullptr,
                       std::string reranker_model_dir = "",
                       std::string query_complexity_model_dir = "models/query-complexity",
                       int candidate_multiplier = 3,
                       int max_candidates = 50,
                       std::string reranker_execution_provider = "auto");
    ~CrossNsQueryWiring();

    CrossNsQueryWiring(const CrossNsQueryWiring&) = delete;
    CrossNsQueryWiring& operator=(const CrossNsQueryWiring&) = delete;

    /// False when a configured reranker model/provider failed to initialize.
    bool IsReady() const;

    /// Mount POST /api/v1/query on `svr`, gated by `auth` (kPermRead). The handler
    /// runs the F04 CrossNsQueryHandler and serializes its §2.5 / §2.6 body.
    void Register(httplib::Server& svr, ApiKeyAuth& auth);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace cortrix::query
