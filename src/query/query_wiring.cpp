#include "cortrix/query/query_wiring.h"

#include <cstdlib>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include <algorithm>
#include <optional>
#include <vector>

#include "cortrix/agent_friendly/error.h"
#include "cortrix/agent_trace/engine_instrumentation.h"  // F13 §11 Engine instrumentation (S6)
#include "cortrix/auth/auth_middleware.h"
#include "cortrix/query/complexity_config.h"
#include "cortrix/query/cross_ns_error.h"
#include "cortrix/query/cross_ns_query_handler.h"
#include "cortrix/query/heuristic_complexity_backend.h"
#include "cortrix/query/live_single_unit_executor.h"
#include "cortrix/query/onnx_complexity_backend.h"
#include "cortrix/query/permission_service.h"
#include "cortrix/query/query_complexity_classifier.h"
#include "cortrix/query/query_context.h"
#include "cortrix/query/query_context_explain.h"
#include "cortrix/query/query_router_metrics.h"
#include "cortrix/query/crag_stage.h"
#include "cortrix/query/query_variant_generator.h"
#include "cortrix/query/llm_rerank_stage.h"
#include "cortrix/query/llm_rerank_types.h"
#include "cortrix/query/rag_fusion.h"
#include "cortrix/query/rag_fusion_stage.h"
#include "cortrix/query/rag_fusion_types.h"
#include "cortrix/query/scatter_gather.h"
#include "cortrix/retrieval/crag_config.h"
#include "cortrix/retrieval/crag_evaluator.h"
#include "cortrix/retrieval/heuristic_guard_backend.h"
#include "cortrix/retrieval/sparse_index_registry.h"
#include "cortrix/common/executor_engine.h"
#include "cortrix/llm/i_llm_client.h"
#include "cortrix/logging/logging.h"
#include "cortrix/memory/memory_block_adapter.h"
#include "cortrix/resource/namespace_facade.h"
#include "cortrix/reranker.h"
#include "cortrix/reranker/reranker_config.h"
#include "cortrix/server/http_server.h"
#include "cortrix/spc/onnx_embedder.h"
#include "cortrix/tenant/permission_service.h"

namespace cortrix::query {

namespace {

// Adapt the P09 tenant PermissionService (cortrix::tenant) onto F04's minimal
// cortrix::query::PermissionService contract (§4.2). The tenant service keys its
// checks off a full AuthContext; F04 hands us (user_id, tenant_id, role) strings,
// so we reconstruct a minimal AuthContext from them. In the CE single-tenant OSS
// hot path (P09 sec 4.6) CanRead returns true once owner_tenant_id == caller
// tenant_id, so the reconstructed context (carrying user_id + tenant_id) is
// sufficient; the full ACL path activates with multi-tenant data.
class TenantPermissionAdapter : public PermissionService {
public:
    TenantPermissionAdapter(cortrix::tenant::PermissionService& svc,
                            cortrix::resource::INamespacePool& pool)
        : svc_(svc), pool_(pool) {}

    /// R1 (Derek 2026-06-12, F04 v1.0.4): with auth disabled the deployment is
    /// CE single-tenant - the injected "default" principal owns every namespace,
    /// so the ns_acl lookup (which has no rows without P09 tenancy) must not
    /// unauthorize it. Set from CrossNsQueryWiring::Register(auth.enabled()).
    void set_auth_enabled(bool enabled) { auth_enabled_ = enabled; }

    BatchCheckResult BatchCheck(const std::string& user_id,
                                const std::string& tenant_id,
                                const std::string& /*role*/,
                                const std::vector<std::string>& namespaces,
                                PermissionAction /*action*/) override {
        AuthContext ctx;
        ctx.user_id = user_id;
        ctx.tenant_id = tenant_id;
        ctx.permissions = kPermRead;  // QUERY == read

        BatchCheckResult result;
        if (!auth_enabled_) return result;  // CE no-auth: single tenant reads all
        std::vector<cortrix::tenant::PermissionService::BatchPermissionCheck> checks =
            svc_.BatchCheck(ctx, namespaces);
        for (const auto& c : checks) {
            if (!c.can_read) result.unauthorized.push_back(c.ns_id);
        }
        return result;
    }

    std::vector<std::string> ListAuthorizedNamespaces(
        const std::string& user_id, const std::string& tenant_id,
        const std::string& /*role*/) override {
        // ["*"] universe = the resident NS set (pool ExplainState), filtered by
        // CanRead. Phase-1 CE: a single default tenant owns every NS, so this
        // returns all active namespaces; multi-tenant ACL filtering is the same
        // BatchCheck path applied per-NS.
        AuthContext ctx;
        ctx.user_id = user_id;
        ctx.tenant_id = tenant_id;
        ctx.permissions = kPermRead;
        std::vector<std::string> active = pool_.GetExplainState().active_namespaces;
        if (active.empty()) return active;
        if (!auth_enabled_) return active;  // CE no-auth: wildcard = all active NS
        std::vector<cortrix::tenant::PermissionService::BatchPermissionCheck> checks =
            svc_.BatchCheck(ctx, active);
        std::vector<std::string> out;
        out.reserve(checks.size());
        for (const auto& c : checks) {
            if (c.can_read) out.push_back(c.ns_id);
        }
        return out;
    }

private:
    bool auth_enabled_ = true;
    cortrix::tenant::PermissionService& svc_;
    cortrix::resource::INamespacePool& pool_;
};

// Build the F02 reranker config from the supplied model dir (resolved from
// config.reranker.model_dir before this call). Empty dir = stub mode (no
// cross-encoder; OnnxReranker falls back to deterministic stub scoring).
reranker::RerankerConfig MakeRerankerConfig(const std::string& model_dir,
                                            const std::string& execution_provider) {
    reranker::RerankerConfig cfg;
    if (!model_dir.empty()) {
        cfg.model_path = model_dir + "/model.onnx";
        cfg.tokenizer_path = model_dir + "/tokenizer.json";
    }
    cfg.execution_provider = execution_provider;
    return cfg;
}

// Build the F39 complexity-classifier backend from the supplied model dir
// (resolved from config.query_complexity.model_dir before this call).
// Init() is offline-friendly: returns Ok + unavailable when the model files
// are absent, falling back to HeuristicComplexityBackend so the routing link
// works without the model (CI / offline).
std::shared_ptr<IComplexityClassifierBackend> MakeComplexityBackend(
    const std::string& model_dir) {
    auto onnx = std::make_shared<OnnxComplexityBackend>(
        model_dir + "/model.onnx", model_dir + "/vocab.txt", /*max_seq_length=*/64);
    onnx->Init();  // Ok + unavailable when the model is absent (offline-friendly)
    if (onnx->IsAvailable()) {
        CORTRIX_LOG_INFO("main", "F39 complexity router: OnnxComplexityBackend (model={})",
                         model_dir);
        return onnx;
    }
    CORTRIX_LOG_INFO("main",
                     "F39 complexity router: model unavailable ({}), using heuristic backend",
                     model_dir);
    return std::make_shared<HeuristicComplexityBackend>();
}

}  // namespace

struct CrossNsQueryWiring::Impl {
    Impl(cortrix::resource::INamespacePool& pool, cortrix::OnnxEmbedder& embedder,
         RRFFusion& fusion, cortrix::tenant::PermissionService& perm_svc,
         cortrix::retrieval::SparseIndexRegistry* sparse_registry,
         std::shared_ptr<cortrix::llm::ILlmClient> llm,
         std::shared_ptr<agent_trace::EngineInstrumentation> engine_instr,
         const std::string& reranker_model_dir,
         const std::string& query_complexity_model_dir,
         int candidate_multiplier,
         int max_candidates,
         const std::string& reranker_execution_provider)
        : pool(pool),
          engine_instr(std::move(engine_instr)),
          perm_adapter(perm_svc, pool),
          // The shared fan-out pool (F04 §2.7 executor.workers / queue_size defaults).
          engine(8, 500),
          // Shared F02 reranker. model_dir from config.reranker.model_dir; empty = stub.
          reranker(reranker::CreateReranker(MakeRerankerConfig(
                                                reranker_model_dir,
                                                reranker_execution_provider),
                                            /*chunk_store=*/nullptr)),
          // F40 per-NS SPLADE index owner (Q4 read path). The SAME registry instance
          // the SPC ingest write path indexes into (bootstrap owns it), so the read
          // path serves exactly what was indexed; null → sparse off (dense+FTS5).
          executor(pool, embedder, fusion, reranker, sparse_registry,
                   candidate_multiplier, max_candidates),
          scatter(&executor, &engine, reranker, &perm_adapter),
          handler(&scatter),
          // F39 query-complexity router (Q3). Standalone backend = the heuristic
          // guard; the real DistilBERT OnnxComplexityBackend
          // (models/query-complexity/) is preferred and drops in behind the same
          // interface (R4 — #26), with the heuristic as the offline fallback
          // (MakeComplexityBackend). The classifier is total (never throws): a
          // missing/failed backend degrades to Complex (L2 "classifier_unavailable"),
          // so the link is safe with either backend.
          classifier(MakeComplexityBackend(query_complexity_model_dir), ComplexityConfig{}),
          // F36 RAG-Fusion (Q5). The variant generator needs an LLM; when none is
          // configured (`llm` null) ExpandQueries can't expand → F36 is effectively
          // off (it is also default-disabled per NS config). rag_rrf is a dedicated
          // RRFFusion for the F36 global second-pass fusion (over the ChildId
          // keyspace), distinct from the inner per-NS RRF.
          variant_generator(std::make_shared<QueryVariantGenerator>(llm)),
          rag_rrf(std::make_shared<RRFFusion>()),
          rag_fusion(variant_generator, rag_rrf),
          rag_stage(&scatter, &rag_fusion, reranker),
          // F37 CRAG evaluator (Q6). Standalone backend = the heuristic guard; the
          // real DistilBERT-tiny OnnxCragBackend drops in behind the same interface
          // (R4). The evaluator is total: a missing/failed backend degrades to the
          // "correct" path, so the result set is never wrongly truncated.
          crag_evaluator(std::make_shared<retrieval::HeuristicGuardBackend>(),
                         retrieval::CragConfig{}),
          crag_stage(&crag_evaluator),
          // F36-LR LLM listwise rerank (addendum §2). Shares the F03 LLM client
          // with F36; null llm → the wiring gate skips the stage (llm_rerank=true
          // then degrades to the pre-stage order, mirroring the F36 [R7] stance).
          llm_rerank_stage(llm) {}

    cortrix::resource::INamespacePool& pool;  ///< for the chat path's per-NS user_facts
    /// F13 Engine instrumentation (§11, S6). null when tracing is off (standalone /
    /// tests). Declared right after `pool` so it is initialized early (its init in the
    /// ctor list is order-independent — it depends on nothing else).
    std::shared_ptr<agent_trace::EngineInstrumentation> engine_instr;
    TenantPermissionAdapter perm_adapter;
    ExecutorEngine engine;
    reranker::IReranker* reranker = nullptr;  // owned (CreateReranker → raw new); freed in dtor
    LiveSingleUnitExecutor executor;
    ScatterGather scatter;
    CrossNsQueryHandler handler;
    QueryComplexityClassifier classifier;
    std::shared_ptr<QueryVariantGenerator> variant_generator;
    std::shared_ptr<RRFFusion> rag_rrf;
    RagFusion rag_fusion;
    RagFusionStage rag_stage;
    retrieval::CragEvaluator crag_evaluator;
    CragStage crag_stage;
    LlmRerankStage llm_rerank_stage;
};

CrossNsQueryWiring::CrossNsQueryWiring(cortrix::resource::INamespacePool& pool,
                                       cortrix::OnnxEmbedder& embedder,
                                       RRFFusion& fusion,
                                       cortrix::tenant::PermissionService& perm_svc,
                                       cortrix::retrieval::SparseIndexRegistry* sparse_registry,
                                       std::shared_ptr<cortrix::llm::ILlmClient> llm,
                                       std::shared_ptr<agent_trace::EngineInstrumentation> engine_instr,
                                       std::string reranker_model_dir,
                                       std::string query_complexity_model_dir,
                                       int candidate_multiplier,
                                       int max_candidates,
                                       std::string reranker_execution_provider)
    : impl_(std::make_unique<Impl>(pool, embedder, fusion, perm_svc,
                                   sparse_registry, std::move(llm),
                                   std::move(engine_instr),
                                   reranker_model_dir,
                                   query_complexity_model_dir,
                                   candidate_multiplier,
                                   max_candidates,
                                   reranker_execution_provider)) {}

CrossNsQueryWiring::~CrossNsQueryWiring() {
    if (impl_ && impl_->reranker) delete impl_->reranker;
}

bool CrossNsQueryWiring::IsReady() const {
    return impl_ != nullptr && impl_->reranker != nullptr;
}

namespace {

// Read the optional Agent ?route override (auto|simple|complex|chat). An invalid
// token is left as-is so RouteAndUpdateContext returns the Agent-friendly
// CX_ERR_F39_FORCE_ROUTE_INVALID and the closure can surface a 400.
std::optional<std::string> ReadRouteOverride(const httplib::Request& req,
                                             const json& body) {
    if (req.has_param("route")) return req.get_param_value("route");  // query-string wins
    if (body.is_object() && body.contains("route") && body["route"].is_string()) {
        return body["route"].get<std::string>();
    }
    return std::nullopt;
}

// Read the F41 ?granularity value (query-string wins over the JSON body, default
// "auto") — same precedence as ?route / ?explain on this path. Validation is the
// caller's (an invalid value is a generic 400, mirroring the single-NS
// query_routes.cpp path: the frozen CX_ERR_F41_* set has no request-param identity).
std::string ReadGranularity(const httplib::Request& req, const json& body) {
    if (req.has_param("granularity")) return req.get_param_value("granularity");
    if (body.is_object() && body.contains("granularity") && body["granularity"].is_string()) {
        return body["granularity"].get<std::string>();
    }
    return "auto";
}

bool IsValidGranularity(const std::string& g) {
    return g == "auto" || g == "chunk" || g == "doc" || g == "both";
}

std::optional<bool> ParseBoolToken(const std::string& v) {
    if (v == "true" || v == "TRUE" || v == "True" || v == "1") return true;
    if (v == "false" || v == "FALSE" || v == "False" || v == "0") return false;
    return std::nullopt;
}

Status ReadCragEnabled(const httplib::Request& req, const json& body, bool* out) {
    if (out == nullptr) return Status::InvalidArgument("crag output pointer is null");
    *out = true;
    if (body.is_object() && body.contains("crag")) {
        if (!body["crag"].is_boolean()) {
            return Status::InvalidArgument("crag must be a boolean when supplied in body");
        }
        *out = body["crag"].get<bool>();
    }
    if (req.has_param("crag")) {
        std::optional<bool> parsed = ParseBoolToken(req.get_param_value("crag"));
        if (!parsed.has_value()) {
            return Status::InvalidArgument("crag must be one of: true, false, 1, 0");
        }
        *out = *parsed;
    }
    return Status::Ok();
}

// Build the per-request QueryContext that carries both the F04 execution fields
// (mirrors ScatterGather::MakeContext) and the F39 routing decision, so the
// per-NS executors run with the resolved route + the request's top_k/rerank/filter.
QueryContext MakeRoutingContext(const json& body, const AuthContext& auth) {
    QueryContext qctx;
    if (body.is_object()) {
        if (body.contains("query") && body["query"].is_string())
            qctx.query = body["query"].get<std::string>();
        if (body.contains("top_k") && body["top_k"].is_number_integer())
            qctx.top_k = body["top_k"].get<int>();
        if (body.contains("rerank") && body["rerank"].is_boolean())
            qctx.rerank = body["rerank"].get<bool>();
        if (body.contains("crag") && body["crag"].is_boolean())
            qctx.enable_crag = body["crag"].get<bool>();
        if (body.contains("search_config") && body["search_config"].is_object()) {
            const auto& sc = body["search_config"];
            if (sc.contains("enable_vector") && sc["enable_vector"].is_boolean())
                qctx.enable_vector = sc["enable_vector"].get<bool>();
            if (sc.contains("enable_bm25") && sc["enable_bm25"].is_boolean())
                qctx.enable_bm25 = sc["enable_bm25"].get<bool>();
            if (sc.contains("enable_sparse") && sc["enable_sparse"].is_boolean())
                qctx.enable_sparse = sc["enable_sparse"].get<bool>();
        }
        if (body.contains("filter") && body["filter"].is_object()) {
            for (auto it = body["filter"].begin(); it != body["filter"].end(); ++it) {
                if (it.value().is_string()) qctx.filter[it.key()] = it.value().get<std::string>();
            }
        }
    }
    if (qctx.top_k < 1) qctx.top_k = 1;
    qctx.user_id = auth.user_id;
    qctx.tenant_id = auth.tenant_id;
    return qctx;
}

// Map a routing_path to the router metric decision label.
QueryRouterMetrics::Decision DecisionOf(const std::string& routing_path,
                                        const std::string& source) {
    if (source == "low_confidence_fallback" || source == "inference_failed_fallback" ||
        source == "classifier_unavailable") {
        return QueryRouterMetrics::Decision::kFallback;
    }
    if (routing_path == "simple") return QueryRouterMetrics::Decision::kSimple;
    if (routing_path == "chat") return QueryRouterMetrics::Decision::kChat;
    return QueryRouterMetrics::Decision::kComplex;
}

// The F39 Chat-path response (§9.1 / F39-5 A): Chat skips ALL retrieval and returns
// only MEM02 user_facts + meta.via_path = "chat_memory_only". User facts are
// per-user but stored per-NS, so we acquire each requested NS facade and query its
// store for the user's active facts (MEM02 QueryUserFacts — pure SQL, no vector
// search), aggregate across NS, sort created_at DESC, and cap at top_k.
json BuildChatResponse(cortrix::resource::INamespacePool& pool, const QueryContext& qctx,
                       const std::vector<std::string>& namespaces) {
    const int limit = qctx.top_k < 1 ? 1 : qctx.top_k;

    std::vector<cortrix::memory::UserFact> facts;
    std::vector<std::string> succeeded;
    std::vector<std::string> queried = namespaces;
    for (const auto& ns : namespaces) {
        cortrix::resource::NamespaceFacade facade(pool, ns);
        if (!facade.Acquire().ok()) continue;  // skip an unresolvable NS (best-effort)
        auto r = cortrix::memory::QueryUserFacts(facade.store(), qctx.user_id, limit);
        if (!r.ok()) continue;
        succeeded.push_back(ns);
        for (auto& f : r.value()) facts.push_back(std::move(f));
    }

    // Newest first across all NS, then cap to top_k (each NS already capped, so the
    // union is at most |NS|*limit before this final cut).
    std::sort(facts.begin(), facts.end(),
              [](const cortrix::memory::UserFact& a, const cortrix::memory::UserFact& b) {
                  return a.created_at > b.created_at;  // ISO-8601 string compare = chrono order
              });
    if (static_cast<int>(facts.size()) > limit) {
        facts.resize(static_cast<std::size_t>(limit));
    }

    json out;
    json results = json::array();
    for (const auto& f : facts) {
        results.push_back({{"child_id", f.block_id},
                           {"content", f.content},
                           {"memory_type", f.memory_type},
                           {"created_at", f.created_at},
                           {"confidence", f.confidence}});
    }
    out["results"] = std::move(results);
    json meta;
    meta["namespaces_queried"] = queried;
    meta["namespaces_succeeded"] = succeeded;
    meta["namespaces_failed"] = json::array();
    meta["coverage_ratio"] = queried.empty()
                                 ? 1.0
                                 : static_cast<double>(succeeded.size()) / queried.size();
    meta["latency_ms"] = 0;
    meta["deduplicated_chunks"] = json::array();
    meta["deduplicated_chunks_count"] = 0;
    meta["warnings"] = json::array();
    meta["via_path"] = "chat_memory_only";  // F39 §9.1 Chat-path marker
    out["meta"] = std::move(meta);
    return out;
}

// Resolve whether F36 RAG-Fusion runs for this request (Q5). NS-config resolution
// (the rag_fusion_config JSONB) is itself D3.5; until that resolver is wired, F36 is
// off by default (topic 3) and an Agent opts in per-request via `rag_fusion: true`
// (body) or `?rag_fusion=true`. variant_count / rrf_k keep the design defaults.
RagFusionConfig ResolveRagFusionConfig(const httplib::Request& req, const json& body) {
    RagFusionConfig cfg;  // enabled = false default (topic 3)
    auto apply_object = [&cfg](const json& obj) {
        if (!obj.is_object()) return;
        if (obj.contains("enabled") && obj["enabled"].is_boolean()) {
            cfg.enabled = obj["enabled"].get<bool>();
        }
        if (obj.contains("variant_count") && obj["variant_count"].is_number_integer()) {
            cfg.variant_count = obj["variant_count"].get<int>();
        }
        if (obj.contains("rrf_k") && obj["rrf_k"].is_number_integer()) {
            cfg.rrf_k = obj["rrf_k"].get<int>();
        }
        if (obj.contains("timeout_ms") && obj["timeout_ms"].is_number_integer()) {
            cfg.timeout_ms = obj["timeout_ms"].get<int64_t>();
        }
        if (obj.contains("locale") && obj["locale"].is_string()) {
            const std::string locale = obj["locale"].get<std::string>();
            if (locale == "en" || locale == "zh") cfg.locale = locale;
        }
        if (obj.contains("model") && obj["model"].is_string()) {
            cfg.model = obj["model"].get<std::string>();
        }
        if (obj.contains("candidate_multiplier") &&
            obj["candidate_multiplier"].is_number_integer()) {
            cfg.candidate_multiplier = obj["candidate_multiplier"].get<int>();
        }
        if (obj.contains("max_candidates") && obj["max_candidates"].is_number_integer()) {
            cfg.max_candidates = obj["max_candidates"].get<int>();
        }
        if (obj.contains("final_rerank") && obj["final_rerank"].is_boolean()) {
            cfg.final_rerank = obj["final_rerank"].get<bool>();
        }
        if (obj.contains("activation_policy") && obj["activation_policy"].is_string()) {
            cfg.activation_policy = obj["activation_policy"].get<std::string>();
        }
        if (obj.contains("activation_score_margin") &&
            obj["activation_score_margin"].is_number()) {
            cfg.activation_score_margin = obj["activation_score_margin"].get<float>();
        }
        if (obj.contains("activation_min_results") &&
            obj["activation_min_results"].is_number_integer()) {
            cfg.activation_min_results = obj["activation_min_results"].get<int>();
        }
        if (obj.contains("fusion_original_weight") &&
            obj["fusion_original_weight"].is_number()) {
            cfg.fusion_original_weight = obj["fusion_original_weight"].get<float>();
        }
        if (obj.contains("fusion_variant_weight") &&
            obj["fusion_variant_weight"].is_number()) {
            cfg.fusion_variant_weight = obj["fusion_variant_weight"].get<float>();
        }
        if (obj.contains("fusion_anchor_max") &&
            obj["fusion_anchor_max"].is_number_integer()) {
            cfg.fusion_anchor_max = obj["fusion_anchor_max"].get<int>();
        }
    };

    if (req.has_param("rag_fusion")) {
        const std::string v = req.get_param_value("rag_fusion");
        cfg.enabled = (v == "true" || v == "1");
    } else if (body.is_object() && body.contains("rag_fusion") &&
               body["rag_fusion"].is_boolean()) {
        cfg.enabled = body["rag_fusion"].get<bool>();
    }
    auto maybe_set_locale = [&cfg](const std::string& locale) {
        if (locale == "en" || locale == "zh") cfg.locale = locale;
    };
    if (body.is_object() && body.contains("locale") && body["locale"].is_string()) {
        maybe_set_locale(body["locale"].get<std::string>());
    }
    if (body.is_object() && body.contains("rag_fusion_config")) {
        apply_object(body["rag_fusion_config"]);
    }
    if (req.has_param("locale")) {
        maybe_set_locale(req.get_param_value("locale"));
    }
    if (req.has_param("rag_fusion_candidate_multiplier")) {
        try {
            cfg.candidate_multiplier =
                std::stoi(req.get_param_value("rag_fusion_candidate_multiplier"));
        } catch (...) {
        }
    }
    if (req.has_param("rag_fusion_max_candidates")) {
        try {
            cfg.max_candidates = std::stoi(req.get_param_value("rag_fusion_max_candidates"));
        } catch (...) {
        }
    }
    if (req.has_param("rag_fusion_final_rerank")) {
        const std::string v = req.get_param_value("rag_fusion_final_rerank");
        cfg.final_rerank = (v == "true" || v == "1");
    }
    if (req.has_param("rag_fusion_activation_policy")) {
        cfg.activation_policy = req.get_param_value("rag_fusion_activation_policy");
    }
    if (req.has_param("rag_fusion_activation_score_margin")) {
        try {
            cfg.activation_score_margin =
                std::stof(req.get_param_value("rag_fusion_activation_score_margin"));
        } catch (...) {
        }
    }
    if (req.has_param("rag_fusion_activation_min_results")) {
        try {
            cfg.activation_min_results =
                std::stoi(req.get_param_value("rag_fusion_activation_min_results"));
        } catch (...) {
        }
    }
    return cfg;
}

Status ValidateResolvedRagFusionConfigForRequest(const RagFusionConfig& cfg) {
    std::string field;
    std::string valid_range;
    if (ValidateRagFusionConfig(cfg, &field, &valid_range)) {
        return Status::Ok();
    }
    return Status::InvalidArgument(
        "CX_ERR_RAG_FUSION_CONFIG_INVALID: rag_fusion_config." + field +
        " must be " + valid_range);
}

// Resolve the F36-LR LLM listwise rerank config (addendum §2.1). Default off;
// an Agent opts in per request via `llm_rerank: true` (body) or `?llm_rerank=true`,
// with knobs in the `llm_rerank_config` body object / `llm_rerank_*` params.
// Mirrors ResolveRagFusionConfig so the two LLM-dependent features stay uniform.
LlmRerankConfig ResolveLlmRerankConfig(const httplib::Request& req, const json& body) {
    LlmRerankConfig cfg;  // enabled = false default
    auto apply_object = [&cfg](const json& obj) {
        if (!obj.is_object()) return;
        if (obj.contains("enabled") && obj["enabled"].is_boolean()) {
            cfg.enabled = obj["enabled"].get<bool>();
        }
        if (obj.contains("top_n") && obj["top_n"].is_number_integer()) {
            cfg.top_n = obj["top_n"].get<int>();
        }
        if (obj.contains("max_doc_chars") && obj["max_doc_chars"].is_number_integer()) {
            cfg.max_doc_chars = obj["max_doc_chars"].get<int>();
        }
        if (obj.contains("timeout_ms") && obj["timeout_ms"].is_number_integer()) {
            cfg.timeout_ms = obj["timeout_ms"].get<int64_t>();
        }
        if (obj.contains("model") && obj["model"].is_string()) {
            cfg.model = obj["model"].get<std::string>();
        }
        if (obj.contains("locale") && obj["locale"].is_string()) {
            const std::string locale = obj["locale"].get<std::string>();
            if (locale == "en" || locale == "zh") cfg.locale = locale;
        }
        if (obj.contains("consensus_runs") &&
            obj["consensus_runs"].is_number_integer()) {
            cfg.consensus_runs = obj["consensus_runs"].get<int>();
        }
    };

    if (req.has_param("llm_rerank")) {
        const std::string v = req.get_param_value("llm_rerank");
        cfg.enabled = (v == "true" || v == "1");
    } else if (body.is_object() && body.contains("llm_rerank") &&
               body["llm_rerank"].is_boolean()) {
        cfg.enabled = body["llm_rerank"].get<bool>();
    }
    if (body.is_object() && body.contains("llm_rerank_config")) {
        apply_object(body["llm_rerank_config"]);
    }
    if (req.has_param("llm_rerank_top_n")) {
        try {
            cfg.top_n = std::stoi(req.get_param_value("llm_rerank_top_n"));
        } catch (...) {
        }
    }
    if (req.has_param("llm_rerank_model")) {
        cfg.model = req.get_param_value("llm_rerank_model");
    }
    if (req.has_param("llm_rerank_timeout_ms")) {
        try {
            cfg.timeout_ms = std::stoll(req.get_param_value("llm_rerank_timeout_ms"));
        } catch (...) {
        }
    }
    if (req.has_param("llm_rerank_consensus_runs")) {
        try {
            cfg.consensus_runs =
                std::stoi(req.get_param_value("llm_rerank_consensus_runs"));
        } catch (...) {
        }
    }
    return cfg;
}

Status ValidateResolvedLlmRerankConfigForRequest(const LlmRerankConfig& cfg) {
    std::string field;
    std::string valid_range;
    if (ValidateLlmRerankConfig(cfg, &field, &valid_range)) {
        return Status::Ok();
    }
    return Status::InvalidArgument(
        "CX_ERR_LLM_RERANK_CONFIG_INVALID: llm_rerank_config." + field +
        " must be " + valid_range);
}

// [F13 §11 / S6] Record one finished query as an agent_trace row via the Engine
// instrumentation. No-op when tracing is off (engine_instr null → standalone/tests).
// The helper itself never throws (EngineInstrumentation::Record isolates both
// writes, C4), so it is safe to call on the request path. Identity
// (trace_id/session_id/agent_id/user_id) is read from the thread-local
// ObservabilityContext WithAuth installed (§5.1); a cross-NS query stamps no single
// namespace_id (NULL = global call, §4.1). `params_summary` is a compact JSON
// (truncated to ≤2KB on write); on failure the error code + message are kept.
//
// op_summary (the F18a operation_log summary, §11 — query_text prefix) is filled too,
// but the operation_log write happens ONLY when the bootstrap constructs this
// EngineInstrumentation WITH an operation_logger. This cell wires agent_trace, so the
// recommended query instance is trace-only (op_logger null) → Record skips the F18a
// block and op_summary is unused; the field is set so enabling op_logger later is
// correct without touching this code.
void RecordQueryTrace(agent_trace::EngineInstrumentation* engine_instr,
                      const std::string& params_summary, int duration_ms,
                      bool is_success, const std::string& result_summary,
                      const std::string& query_text_summary,
                      const std::optional<std::string>& error_code,
                      const std::string& error_message) {
    if (engine_instr == nullptr) return;
    agent_trace::EngineCall call;
    call.method = "query";
    call.source = "http";
    call.params_json = params_summary;
    call.duration_ms = duration_ms;
    call.is_success = is_success;
    call.op_summary = query_text_summary;  // §11 operation_log summary (op_logger-gated)
    if (is_success) {
        call.result_summary = result_summary;
    } else {
        call.error_code = error_code;
        call.error_message = error_message;
    }
    engine_instr->Record(call);
}

// First 100 chars of the query text — the F18a operation_log summary shape (§11).
std::string QueryTextSummary(const QueryContext& qctx) {
    return qctx.query.size() > 100 ? qctx.query.substr(0, 100) : qctx.query;
}

// Build the compact params JSON for the trace row (§4.1 — params ≤2KB, the writer
// truncates). Keeps just the routing-relevant request shape (query text is the
// Agent's own input; top_k / route / granularity describe the call), not the full
// body, so a trace row stays small and PII-light.
std::string BuildQueryParamsSummary(const QueryContext& qctx) {
    json p;
    p["query"] = qctx.query;
    p["top_k"] = qctx.top_k;
    p["route"] = qctx.routing_path;
    p["granularity"] = qctx.granularity;
    p["crag"] = qctx.enable_crag;
    return p.dump();
}

// Serialize a scatter response, applying the F37 CRAG verdict action + surfacing
// meta.crag_verdict (B-class) when F37 ran (Complex route). CragStage is a no-op on
// simple/chat (ShouldSkipF37) so this is safe to call on every scatter path.
json SerializeWithCrag(CrossNsResponse& resp, QueryContext& qctx, CragStage* crag) {
    crag->Apply(resp, qctx);
    json out = resp.ToJson();
    if (!qctx.crag_verdict.empty()) {
        out["meta"]["crag_verdict"] = qctx.crag_verdict;  // F37 §6.3 B-class
        if (!qctx.ambiguous_action_taken.empty()) {
            out["meta"]["crag_action"] = qctx.ambiguous_action_taken;
        }
    }
    return out;
}

}  // namespace

void CrossNsQueryWiring::Register(httplib::Server& svr, ApiKeyAuth& auth) {
    // R1: propagate the deployment auth posture into the F04 permission adapter
    // (CE no-auth single tenant must not be unauthorized by the empty ns_acl).
    impl_->perm_adapter.set_auth_enabled(auth.enabled());
    CrossNsQueryHandler* handler = &impl_->handler;
    QueryComplexityClassifier* classifier = &impl_->classifier;
    RagFusionStage* rag_stage = &impl_->rag_stage;
    RagFusion* rag_fusion = &impl_->rag_fusion;
    ScatterGather* scatter = &impl_->scatter;
    CragStage* crag_stage = &impl_->crag_stage;
    cortrix::resource::INamespacePool* pool = &impl_->pool;
    // F13 §11 Engine instrumentation (raw ptr; Impl outlives the closure). null →
    // tracing off; RecordQueryTrace then no-ops, leaving the path unchanged.
    agent_trace::EngineInstrumentation* engine_instr = impl_->engine_instr.get();
    // [R7] Whether an LLM is configured — F36 rag-fusion needs one to expand query
    // variants. When absent (CE OSS default) the gate below skips rag-fusion and
    // runs plain scatter, so a `rag_fusion=true` request degrades gracefully instead
    // of dereferencing a null LLM client (the §F36 LLM-unavailable contract).
    const bool rag_fusion_llm_available = impl_->variant_generator->has_llm();
    // F36-LR listwise rerank stage (addendum §2). Same availability stance: no
    // LLM configured → the gate below skips the stage and the response keeps the
    // pre-stage (F02 cross-encoder) order.
    LlmRerankStage* llm_rerank_stage = &impl_->llm_rerank_stage;
    const bool llm_rerank_available = impl_->llm_rerank_stage.has_llm();
    svr.Post("/api/v1/query", WithAuth(auth, kPermRead,
        [handler, classifier, rag_stage, rag_fusion, scatter, crag_stage, pool, engine_instr,
         rag_fusion_llm_available, llm_rerank_stage, llm_rerank_available](
            const httplib::Request& req, httplib::Response& res,
            const RequestContext& ctx) {
            // Parse the JSON body up-front so a malformed body is a clean 400 here
            // (the F04 handler also validates shape, but it expects a JSON object).
            json body;
            try {
                body = json::parse(req.body);
            } catch (...) {
                WriteJsonError(res, Status::InvalidArgument("Invalid JSON in request body"),
                               ctx.request_id);
                return;
            }

            // R1 (Derek 2026-06-12): CE no-auth leaves user_id empty; inject the
            // single-tenant default principal so F04 AuthorizeNamespaces Step 1
            // passes (F04 §4.2 v1.0.4). When auth is enabled the middleware has
            // already populated a real user_id and this is a no-op.
            AuthContext auth_ctx = ctx.auth;
            if (auth_ctx.user_id.empty()) auth_ctx.user_id = "default";

            // F39 routing (Q3, §6.1): classify the query and write routing_path onto
            // the QueryContext BEFORE retrieval. The classifier runs the §6.1 order
            // internally (Agent ?route override → NS force_route → IsChatQuery rule
            // guard → backend Infer → confidence<threshold fail-safe → Complex), so
            // the chat rule guard correctly precedes the ML backend (team-lead note
            // on short-text domain shift). It is total; an invalid ?route returns
            // CX_ERR_F39_FORCE_ROUTE_INVALID and ctx is left untouched.
            QueryContext qctx = MakeRoutingContext(body, auth_ctx);
            qctx.ns_id.clear();  // cross-NS: per-NS id is bound inside the executor

            // F41 §6.2 ?granularity (auto|chunk|doc|both, query-string wins over body).
            // Validate before routing so an invalid value is a clean 400 (the frozen
            // CX_ERR_F41_* set has no request-param identity → generic InvalidArgument,
            // matching the single-NS query_routes.cpp path). "chunk" is the explicit
            // baseline; "auto" and "both" engage the hybrid doc-summary fallback inside
            // LiveSingleUnitExecutor. ScatterGather/Gather are untouched (pass-through).
            qctx.granularity = ReadGranularity(req, body);
            if (!IsValidGranularity(qctx.granularity)) {
                WriteJsonError(res, Status::InvalidArgument(
                    "granularity must be one of: auto, chunk, doc, both"),
                    ctx.request_id);
                return;
            }
            bool crag_enabled = true;
            Status crag_status = ReadCragEnabled(req, body, &crag_enabled);
            if (!crag_status.ok()) {
                WriteJsonError(res, crag_status, ctx.request_id);
                return;
            }
            qctx.enable_crag = crag_enabled;

            std::optional<std::string> route_override = ReadRouteOverride(req, body);
            Status routed = classifier->RouteAndUpdateContext(qctx, route_override);
            if (!routed.ok()) {
                // Invalid ?route token — surface the Agent-friendly 400 directly.
                WriteJsonError(res, routed, ctx.request_id);
                return;
            }
            QueryRouterMetrics::Instance().RecordDecision(
                DecisionOf(qctx.routing_path, qctx.routing_decision_source));

            // [F13 §11] Time the actual query execution (chat / scatter) for the
            // agent_trace duration_ms. Started after routing resolves so it measures
            // the execution, not the pre-execution validation (those 400s are request
            // -shape errors, not Engine calls, and are not traced — matching §11 which
            // instruments DoQuery, and F18a which logs success only).
            const auto exec_start = std::chrono::steady_clock::now();
            auto elapsed_ms = [&exec_start]() -> int {
                return static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - exec_start).count());
            };

            // Chat path (§9.1 / F39-5 A): skip retrieval + F36/F37/F38 entirely,
            // return only MEM02 user_facts + meta.via_path. The deprecated-field
            // (CX_ERR_DEPRECATED_FIELD) check still must run on the raw body, so a
            // chat-routed request that ALSO carries the MVP `namespace` field is
            // rejected before the short-circuit (consistency with the F04 handler).
            // The request must still parse (need the namespaces[] to scope the
            // per-NS user_facts query); a parse fault falls through to Handle for
            // the precise error body.
            if (qctx.routing_path == "chat" && !body.contains("namespace")) {
                QueryRequest chat_req;
                if (CrossNsQueryHandler::ParseRequest(body, &chat_req).ok()) {
                    json chat_body = BuildChatResponse(*pool, qctx, chat_req.namespaces);
                    // [F13 §11] Trace the chat-path query (memory-only retrieval).
                    RecordQueryTrace(engine_instr, BuildQueryParamsSummary(qctx),
                                     elapsed_ms(), /*is_success=*/true,
                                     "chat_memory_only", QueryTextSummary(qctx),
                                     std::nullopt, "");
                    res.status = 200;
                    res.set_header("X-Request-Id", ctx.request_id);
                    res.set_content(chat_body.dump(), "application/json");
                    return;
                }
                // else: fall through to Handle below for the bad-request body.
            }

            // Simple / Complex retrieval path. Parse the F04 request once: on a
            // parse fault (deprecated `namespace`, malformed body) fall through to
            // handler->Handle, which produces the precise CX_ERR_DEPRECATED_FIELD /
            // bad-request body. On success run the scatter (or the F36 RagFusionStage
            // when Complex + rag_fusion enabled), then apply F37 CRAG (Q6) before
            // serializing. CragStage is a no-op on the Simple route (ShouldSkipF37).
            QueryRequest f04_req;
            Status parsed = CrossNsQueryHandler::ParseRequest(body, &f04_req);
            if (parsed.ok()) {
                // ?explain (B/C-class transparency, QUERY_CONTEXT_SPEC §2 / GEN-Agent):
                // the cross-NS request contract (ParseRequest) carries the retrieval
                // fields only, so the explain presentation flag is read here from the
                // body and the query string (truthy "true"/"1") — mirroring the
                // single-NS ApplyExplainParam path (query_routes.cpp).
                bool explain = body.contains("explain") && body["explain"].is_boolean()
                                   ? body["explain"].get<bool>()
                                   : false;
                if (req.has_param("explain")) {
                    const std::string v = req.get_param_value("explain");
                    explain = (v == "true" || v == "TRUE" || v == "True" || v == "1");
                }
                // Pass the presentation flag into the per-NS executor so it attaches
                // the chunk-level RRF `rrf_paths` explain detail (§3.8 W2). Read
                // above from body/param; set before scatter/rag runs (line ~850).
                qctx.explain = explain;
                RagFusionConfig rag_cfg = ResolveRagFusionConfig(req, body);
                Status rag_cfg_status = ValidateResolvedRagFusionConfigForRequest(rag_cfg);
                if (!rag_cfg_status.ok()) {
                    WriteJsonError(res, rag_cfg_status, ctx.request_id);
                    return;
                }
                LlmRerankConfig lr_cfg = ResolveLlmRerankConfig(req, body);
                Status lr_cfg_status = ValidateResolvedLlmRerankConfigForRequest(lr_cfg);
                if (!lr_cfg_status.ok()) {
                    WriteJsonError(res, lr_cfg_status, ctx.request_id);
                    return;
                }
                // [R7] rag-fusion also requires a configured LLM (variant expansion
                // calls it); without one a `rag_fusion=true` request degrades to
                // plain scatter rather than crashing on a null LLM client.
                const bool use_rag_fusion = qctx.routing_path == "complex" &&
                                            rag_cfg.enabled && rag_fusion_llm_available;
                // F36-LR gate (addendum §2): needs an LLM; the listwise window
                // needs top_n candidates, so widen the retrieval top_k and trim
                // back to the Agent's requested top_k after the stage.
                const bool use_llm_rerank = lr_cfg.enabled && llm_rerank_available;
                const int requested_top_k = f04_req.top_k;
                if (use_llm_rerank && f04_req.top_k < lr_cfg.top_n) {
                    f04_req.top_k = lr_cfg.top_n;
                    qctx.top_k = lr_cfg.top_n;
                }
                try {
                    CrossNsResponse resp =
                        use_rag_fusion
                            ? rag_stage->Run(f04_req, auth_ctx, qctx, rag_cfg)
                            : scatter->Execute(f04_req, auth_ctx, &qctx);
                    LlmRerankStage::ExplainState lr_es;
                    if (use_llm_rerank) {
                        lr_es = llm_rerank_stage->Apply(&resp, f04_req.query, lr_cfg);
                        if (static_cast<int>(resp.results.size()) > requested_top_k) {
                            resp.results.resize(
                                static_cast<std::size_t>(requested_top_k));
                        }
                    }
                    // [F13 §11] Trace the executed retrieval query (success). Summary
                    // = result count + coverage (the writer truncates to ≤512).
                    RecordQueryTrace(
                        engine_instr, BuildQueryParamsSummary(qctx), elapsed_ms(),
                        /*is_success=*/true,
                        "results=" + std::to_string(resp.results.size()) +
                            " coverage=" + std::to_string(resp.meta.coverage_ratio),
                        QueryTextSummary(qctx), std::nullopt, "");
                    json out = SerializeWithCrag(resp, qctx, crag_stage);
                    if (explain) {
                        // C-class debug fields surface only when a capability ran AND
                        // failed (SPEC §3); on this path the router's transient-inference
                        // fallback is the only such signal. Reuses the single-NS
                        // BuildExplainNode so the explain schema is identical across
                        // both query paths.
                        const bool include_debug =
                            qctx.routing_decision_source == "inference_failed_fallback";
                        out["explain"] =
                            cortrix::query::BuildExplainNode(qctx, include_debug);
                        // F41 §6.2: echo the resolved granularity so the Agent sees the
                        // effective value (mirrors the single-NS query_routes.cpp echo).
                        out["explain"]["granularity"] = qctx.granularity;
                        // [addendum §3.8 W2 · P5] Aggregate per-path vote counts over
                        // the RETURNED results: rrf_path_counts = how many results each
                        // of the five chunk-level paths helped rank (dense / fts5 /
                        // sparse / contextualized / hype_question); via_path_counts =
                        // doc-route provenance (chunk / hybrid / llm_summary / …). Lets
                        // a caller split the F41 doc-route vs F35/F38 chunk-path
                        // contribution without client-side counting.
                        {
                            json path_counts = json::object();
                            json via_counts = json::object();
                            for (const auto& item : resp.results) {
                                auto rp = item.metadata.find("rrf_paths");
                                if (rp != item.metadata.end()) {
                                    std::stringstream ss(rp->second);
                                    std::string tok;
                                    while (std::getline(ss, tok, ',')) {
                                        if (tok.empty()) continue;
                                        path_counts[tok] =
                                            path_counts.value(tok, 0) + 1;
                                    }
                                }
                                auto vp = item.metadata.find("via_path");
                                if (vp != item.metadata.end() && !vp->second.empty()) {
                                    via_counts[vp->second] =
                                        via_counts.value(vp->second, 0) + 1;
                                }
                            }
                            if (!path_counts.empty()) {
                                out["explain"]["rrf_path_counts"] = path_counts;
                            }
                            if (!via_counts.empty()) {
                                out["explain"]["via_path_counts"] = via_counts;
                            }
                        }
                        if (use_rag_fusion) {
                            const auto es = rag_fusion->GetExplainState();
                            json rf = {
                                {"active", es.active},
                                {"feature_id", "F36"},
                                {"reason", es.reason},
                                {"variant_count", es.variant_count},
                                {"degraded", es.degraded},
                            };
                            if (!es.variants_used.empty()) {
                                rf["variants_used"] = es.variants_used;
                            }
                            if (!es.degrade_reason.empty()) {
                                rf["degrade_reason"] = es.degrade_reason;
                            }
                            if (!es.degrade_detail.empty()) {
                                rf["degrade_detail"] = es.degrade_detail;
                            }
                            if (es.llm_latency_ms.has_value()) {
                                rf["llm_latency_ms"] = *es.llm_latency_ms;
                            }
                            out["explain"]["llm_dependent_features"]["rag_fusion"] =
                                std::move(rf);
                        }
                        if (use_llm_rerank) {
                            // F36-LR explain (addendum §2.5) — B-class, explain-only.
                            json lr = {
                                {"active", lr_es.active},
                                {"feature_id", "F36-LR"},
                                {"reason", lr_es.reason},
                                {"top_n_effective", lr_es.top_n_effective},
                                {"order_changed", lr_es.order_changed},
                                {"degraded", lr_es.degraded},
                            };
                            if (!lr_es.model_used.empty()) {
                                lr["model"] = lr_es.model_used;
                            }
                            if (!lr_es.degrade_reason.empty()) {
                                lr["degrade_reason"] = lr_es.degrade_reason;
                            }
                            if (!lr_es.degrade_detail.empty()) {
                                lr["degrade_detail"] = lr_es.degrade_detail;
                            }
                            if (lr_es.active) {
                                lr["llm_latency_ms"] = lr_es.llm_latency_ms;
                                lr["llm_calls"] = lr_es.llm_calls;
                                lr["votes_ok"] = lr_es.votes_ok;
                            }
                            out["explain"]["llm_dependent_features"]["llm_rerank"] =
                                std::move(lr);
                        }
                    }
                    res.status = 200;
                    res.set_header("X-Request-Id", ctx.request_id);
                    res.set_content(out.dump(), "application/json");
                    return;
                } catch (const CrossNsException& e) {
                    const int http = GetCrossNsErrorInfo(e.code()).http_status;
                    // [F13 §11] Trace the failed query (status=failed + error code).
                    RecordQueryTrace(engine_instr, BuildQueryParamsSummary(qctx),
                                     elapsed_ms(), /*is_success=*/false, "",
                                     QueryTextSummary(qctx), e.GetError().code,
                                     e.GetError().message);
                    json eb;
                    eb["error"] = agent_friendly::ToJson(e.GetError());
                    res.status = http;
                    res.set_header("X-Request-Id", ctx.request_id);
                    res.set_content(eb.dump(), "application/json");
                    return;
                }
            }

            // Parse fault → the F04 handler produces the precise error body.
            HandlerResult hr = handler->Handle(body, auth_ctx, &qctx);
            res.status = hr.status;
            res.set_header("X-Request-Id", ctx.request_id);
            res.set_content(hr.body.dump(), "application/json");
        }
    ));
    CORTRIX_LOG_INFO("main",
                     "F04 cross-NS query + F39 routing mounted on POST /api/v1/query");
}

}  // namespace cortrix::query
