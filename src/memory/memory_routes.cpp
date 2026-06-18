#include <cstdint>
#include "cortrix/memory/memory_routes.h"
#include "cortrix/memory/memory_store.h"
#include "cortrix/memory/memory_writer.h"
#include "cortrix/memory/memory_searcher.h"
#include "cortrix/memory/memory_injector.h"
#include "cortrix/memory/memory_config.h"
#include "cortrix/auth/api_key_auth.h"
#include "cortrix/auth/auth_middleware.h"
#include "cortrix/resource/namespace_facade.h"
#include "cortrix/spc/spc_manager.h"
#include "cortrix/query/query_pipeline.h"
#include "cortrix/query/rrf_fusion.h"
#include "cortrix/query/intent_classifier.h"
#include "cortrix/query/bm25_searcher.h"
#include "cortrix/query/vector_searcher.h"
#include "cortrix/query/degradation_manager.h"
#include "cortrix/query/post_filter.h"
#include "cortrix/query/sql_executor.h"
#include "cortrix/spc/onnx_embedder.h"
#include "cortrix/server/http_server.h"
#include "cortrix/memory/mem05_metrics.h"
// M1/M2/M4/M5 — MEM02 extraction + MEM03 transparency + MEM04 opt-out runtime
#include "cortrix/memory/interaction_log.h"
#include "cortrix/memory/memory_extraction_service.h"
#include "cortrix/memory/memory_extractor.h"
#include "cortrix/memory/memory_block_adapter.h"
#include "cortrix/memory/memory_transparency.h"
#include "cortrix/memory/opt_out_manager.h"
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

namespace cortrix {

using json = nlohmann::json;

// MEM05 §8.bis: resolve the requester's user_id from the `user_id` query param,
// applying the CE no-auth `default` fallback. When the fallback fires it is a
// cortrix_mem05_default_user_used_total event (ops signal: a node running auth-less
// on default). Returns the effective user_id used for the isolation check.
static std::string ResolveRequesterUserId(const httplib::Request& req) {
    std::string req_user_id = req.get_param_value("user_id");
    if (req_user_id.empty()) {
        req_user_id = "default";  // CE no-auth fallback
        memory::Mem05Metrics::Instance().RecordDefaultUserUsed();
    }
    return req_user_id;
}

// MEM05 §8.bis: record one isolation decision at an API entry — the
// isolation_check_total audit baseline plus, on a cross-user denial,
// isolation_violation_total{reason=mismatch} (the safety-critical alert).
static void RecordIsolationDecision(memory::Mem05Metrics::Action action, bool owned) {
    auto& m = memory::Mem05Metrics::Instance();
    m.RecordIsolationCheck(owned ? memory::Mem05Metrics::CheckResult::kPass
                                 : memory::Mem05Metrics::CheckResult::kViolation,
                           action);
    if (!owned) {
        m.RecordIsolationViolation(action, memory::Mem05Metrics::Reason::kMismatch);
    }
}

// MEM05 IDOR guard: resolve the effective user_id from a caller-supplied value
// (body or query param) and decide whether the authenticated principal may act on
// it. A non-admin principal may only target its own user_id — supplying someone
// else's user_id is a body-spoof cross-user attempt and is denied. This is the
// single source of truth behind every memory CRUD route (list/create/edit/delete/
// search), so they all enforce isolation identically (matching the original GET
// list guard at /api/v1/memory).
//
// @param requested  [in/out] the caller-supplied user_id; on entry empty means
//                   "use my own", and it is rewritten to the effective user_id
//                   (auth.user_id, or "default" in CE no-auth mode).
// @return true  = allowed; `requested` holds the effective user_id to use.
//         false = cross-user denial; the caller must emit the route's mask
//                 response (404 for mutations, empty 200 for list/search) and
//                 return. One isolation_violation_total{reason=mismatch} is
//                 recorded here; the caller adds no further metric.
static bool EnforceOwnUserId(const RequestContext& rc, std::string& requested,
                             memory::Mem05Metrics::Action action) {
    if (requested.empty()) {
        requested = rc.auth.user_id.empty() ? "default" : rc.auth.user_id;
    }
    // Admins may target any user_id; a principal with no user_id (e.g. the CE
    // no-auth path where auth.user_id is empty) is not subject to the per-user
    // mismatch check — P08 per-key isolation still applies upstream.
    const bool owned = rc.auth.is_admin() || rc.auth.user_id.empty() ||
                       requested == rc.auth.user_id;
    RecordIsolationDecision(action, owned);
    return owned;
}

// MEM05 IDOR guard for session-scoped operations (inject / write-interaction /
// opt-out). The session's owner is the authoritative user_id (the memory_sessions
// row), so a caller cannot self-assert ownership via the body. A non-admin
// principal may only act on a session it owns; acting on another principal's
// session is denied. Looks the owner up via MemoryStore::SessionGet.
//
// Semantics (anti-enumeration, design § 4.5):
//   - session exists & owned (or admin / no-auth empty principal) → true (proceed).
//   - session exists & owned by another principal → false; the caller emits a 404
//     mask and returns. One isolation_violation_total{reason=mismatch} recorded.
//   - session does NOT exist → true (proceed); the downstream service emits its own
//     not-found, so we neither double-handle it nor leak existence. No metric.
static bool EnforceSessionOwner(const RequestContext& rc, MemoryStore& store,
                                const std::string& session_id,
                                memory::Mem05Metrics::Action action) {
    // Admin and the no-auth empty principal are not subject to the per-user check
    // (P08 per-key isolation still applies upstream); skip the lookup entirely.
    if (rc.auth.is_admin() || rc.auth.user_id.empty()) return true;
    MemorySession session;
    if (!store.SessionGet(session_id, session).ok()) {
        return true;  // unknown session — let the downstream path 404 it.
    }
    const bool owned = (session.user_id == rc.auth.user_id);
    RecordIsolationDecision(action, owned);
    return owned;
}

// Write a GEN-Agent 4-field error envelope (code/retryable/category + structured_data)
// with an explicit HTTP status. Used by the M-wave endpoints whose errors carry a
// CX_ERR_MEM0x_* identity rather than a generic Status.
static void WriteAgentError(httplib::Response& res, int http_status,
                            const std::string& code, const std::string& message,
                            bool retryable, const std::string& category,
                            const json& structured_data = json::object()) {
    json err;
    err["code"] = code;
    err["message"] = message;
    err["retryable"] = retryable;
    err["category"] = category;
    err["structured_data"] = structured_data;
    json body;
    body["error"] = err;
    res.status = http_status;
    res.set_content(body.dump(), "application/json");
}

// ── M2: MEM02 LLM extraction HTTP surface ────────────────────────────────────
// POST /api/v1/memory/extract            — single interaction (PartialSuccessById)
// POST /api/v1/memory/extract/batch      — batch
// POST /api/v1/memory/extract/run_for_ns — NS-level backfill
// GET  /api/v1/memory/invalidations      — D6/D9 audit list
// POST /api/v1/memory/invalidations/{id}/revoke — D9 admin revoke
static void RegisterMemoryExtractRoutes(httplib::Server& svr, ApiKeyAuth& auth,
                                        resource::INamespacePool& pool,
                                        OnnxEmbedder& embedder,
                                        const MemoryConfig& config,
                                        memory::MemoryExtractionService* extraction) {
    (void)pool;
    (void)embedder;
    (void)config;
    auto extract_disabled = [](httplib::Response& res) {
        WriteAgentError(res, 503, "CX_ERR_MEM02_LLM_DISABLED",
                        "memory extraction is disabled (no LLM configured)",
                        false, "permanent");
    };

    // POST /api/v1/memory/extract — extract from one interaction supplied in the body.
    svr.Post("/api/v1/memory/extract", WithAuth(auth, kPermWrite,
        [extraction, extract_disabled](const httplib::Request& req, httplib::Response& res,
                                      const RequestContext& rc) {
        if (!extraction || !extraction->enabled()) { extract_disabled(res); return; }
        json body;
        try { body = json::parse(req.body); } catch (...) {
            WriteAgentError(res, 400, "CX_ERR_MEM02_EXTRACT_INVALID_OUTPUT",
                            "invalid JSON body", false, "permanent");
            return;
        }
        std::string ns = body.value("ns", body.value("namespace", ""));
        if (ns.empty()) { WriteJsonError(res, Status::InvalidArgument("ns is required")); return; }

        InteractionLog turn;
        turn.namespace_name = ns;
        turn.session_id = body.value("session_id", "");
        turn.user_id = body.value("user_id", rc.auth.user_id);
        turn.role = "user";
        // Accept either an explicit content field or a query/response pair.
        if (body.contains("content")) {
            turn.content = body["content"].get<std::string>();
        } else {
            turn.content = "user: " + body.value("query_text", "") +
                           "\nassistant: " + body.value("response_text", "");
        }
        turn.remember = body.value("remember", true);

        auto result = extraction->ExtractOne(turn);
        if (!result.ok()) {
            const auto& e = *result.error;
            WriteAgentError(res, 500, e.code, e.message, false, "transient");
            return;
        }
        json resp;
        resp["extracted_memories"] = json::array();
        for (const auto& m : result.extracted_memories) {
            resp["extracted_memories"].push_back({
                {"block_id", m.block_id},
                {"type", memory::ToString(m.type)},
                {"content", m.content},
                {"confidence", m.confidence},
            });
        }
        if (result.extract_meta) {
            resp["extract_meta"] = {
                {"llm_model", result.extract_meta->llm_model},
                {"contradictions_found", result.extract_meta->contradictions_found},
            };
        }
        resp["error"] = nullptr;
        WriteJsonResponse(res, 200, resp);
    }));

    // POST /api/v1/memory/extract/batch — PartialSuccessById over a list of interactions.
    svr.Post("/api/v1/memory/extract/batch", WithAuth(auth, kPermWrite,
        [extraction, extract_disabled](const httplib::Request& req, httplib::Response& res,
                                      const RequestContext& rc) {
        if (!extraction || !extraction->enabled()) { extract_disabled(res); return; }
        json body;
        try { body = json::parse(req.body); } catch (...) {
            WriteAgentError(res, 400, "CX_ERR_MEM02_EXTRACT_INVALID_OUTPUT",
                            "invalid JSON body", false, "permanent");
            return;
        }
        std::string ns = body.value("ns", body.value("namespace", ""));
        if (ns.empty() || !body.contains("interactions") || !body["interactions"].is_array()) {
            WriteJsonError(res, Status::InvalidArgument("ns + interactions[] are required"));
            return;
        }
        json results = json::array();
        int succeeded = 0, failed = 0;
        for (const auto& it : body["interactions"]) {
            InteractionLog turn;
            turn.namespace_name = ns;
            turn.session_id = it.value("session_id", "");
            turn.user_id = it.value("user_id", rc.auth.user_id);
            turn.role = "user";
            turn.content = it.contains("content") ? it["content"].get<std::string>()
                : ("user: " + it.value("query_text", "") +
                   "\nassistant: " + it.value("response_text", ""));
            turn.remember = it.value("remember", true);
            auto r = extraction->ExtractOne(turn);
            json one;
            one["id"] = turn.session_id;
            one["ok"] = r.ok();
            if (r.ok()) { one["extracted_count"] = r.extracted_memories.size(); ++succeeded; }
            else { one["error"] = r.error->code; ++failed; }
            results.push_back(one);
        }
        json resp;
        resp["results"] = results;
        resp["succeeded"] = succeeded;
        resp["failed"] = failed;
        WriteJsonResponse(res, 200, resp);  // PartialSuccessById: 200 with per-item status
    }));

    // POST /api/v1/memory/extract/run_for_ns — NS-level backfill (scheduled task stub).
    svr.Post("/api/v1/memory/extract/run_for_ns", WithAuth(auth, kPermWrite,
        [extraction, extract_disabled](const httplib::Request& req, httplib::Response& res,
                                      const RequestContext&) {
        if (!extraction || !extraction->enabled()) { extract_disabled(res); return; }
        json body;
        try { body = json::parse(req.body); } catch (...) {
            WriteAgentError(res, 400, "CX_ERR_MEM02_EXTRACT_INVALID_OUTPUT",
                            "invalid JSON body", false, "permanent");
            return;
        }
        std::string ns = body.value("ns", body.value("namespace", ""));
        if (ns.empty()) { WriteJsonError(res, Status::InvalidArgument("ns is required")); return; }
        // The backfill enumerates unextracted interactions and enqueues them; the
        // enumeration source is the MemoryQueue periodic-scan wiring (D2 fallback). Here
        // we accept the request and report it scheduled (async, non-blocking).
        json resp;
        resp["ns"] = ns;
        resp["status"] = "scheduled";
        resp["note"] = "backfill runs through the async extraction queue";
        WriteJsonResponse(res, 202, resp);
    }));

    // GET /api/v1/memory/invalidations — D6/D9 audit list (over operation_log).
    svr.Get("/api/v1/memory/invalidations", WithAuth(auth, kPermRead,
        [](const httplib::Request& req, httplib::Response& res, const RequestContext&) {
        // The invalidation audit trail lives in F18a operation_log (memory_invalidate /
        // memory_revoke actions). Querying it is the /operations surface's job; here we
        // return the canonical pointer so the SDK can follow it (the dedicated filtered
        // view is a thin wrapper deferred to the operations route owner).
        json resp;
        resp["invalidations"] = json::array();
        resp["source"] = "/api/v1/operations?action_in=memory_invalidate,memory_revoke";
        (void)req;
        WriteJsonResponse(res, 200, resp);
    }));

    // POST /api/v1/memory/invalidations/{id}/revoke — D9 admin revoke.
    // Body: { "ns": "<namespace>", "reason": "<why>" }. `ns` is required (the block is
    // NS-scoped; mirrors the opt-out/revoke admin sibling which also reads ns from the
    // body). `revoked_by` is the acting admin's user_id from auth — NEVER the body, so
    // an admin cannot spoof another identity into the audit trail (memory_revoke oplog).
    svr.Post(R"(/api/v1/memory/invalidations/([A-Za-z0-9_\-]+)/revoke)", WithAuth(auth, kPermAdmin,
        [extraction](const httplib::Request& req, httplib::Response& res, const RequestContext& rc) {
        std::string block_id = req.matches[1].str();
        if (!extraction) {
            WriteAgentError(res, 503, "CX_ERR_MEM02_LLM_DISABLED",
                            "memory service unavailable", false, "permanent");
            return;
        }
        json body = json::object();
        try { if (!req.body.empty()) body = json::parse(req.body); } catch (...) {}
        std::string ns = body.value("ns", body.value("namespace", ""));
        if (ns.empty()) { WriteJsonError(res, Status::InvalidArgument("ns is required")); return; }
        std::string reason = body.value("reason", "");
        const std::string revoked_by =
            rc.auth.user_id.empty() ? "anonymous" : rc.auth.user_id;

        auto r = extraction->RevokeInvalidation(ns, block_id, reason, revoked_by);
        if (!r.ok()) {
            // Map the engine/Status to HTTP: NS / block not found → 404, else 500.
            const Status& s = r.status();
            const int http = s.code() == StatusCode::kNotFound ? 404 : 500;
            WriteAgentError(res, http,
                            http == 404 ? "CX_ERR_MEM02_BLOCK_NOT_FOUND"
                                        : "CX_ERR_MEM02_REVOKE_FAILED",
                            s.message(), false, "permanent");
            return;
        }
        const memory::MemoryBlockRecord& blk = r.value();
        json resp;
        resp["block_id"] = blk.block_id.empty() ? block_id : blk.block_id;
        resp["status"] = "active";  // revoke restores the block to active
        resp["revoked_by"] = revoked_by;
        if (blk.metadata_json.is_object() && blk.metadata_json.contains("revoked_at")) {
            resp["revoked_at"] = blk.metadata_json["revoked_at"];
        }
        resp["namespace"] = ns;
        WriteJsonResponse(res, 200, resp);
    }));
}

// ── M4: MEM03 Memory Transparency HTTP surface ───────────────────────────────
// GET    /api/v1/memory       — list a user's memories (MEM05 isolation)
// POST   /api/v1/memory       — create a memory
// PATCH  /api/v1/memory/{id}  — edit (= new fact + invalidate old)
// DELETE /api/v1/memory/{id}  — soft-delete (status=invalidated)
static void RegisterMemoryTransparencyRoutes(httplib::Server& svr, ApiKeyAuth& auth,
                                             resource::INamespacePool& pool,
                                             OnnxEmbedder& embedder,
                                             const MemoryConfig& config,
                                             const MemoryServices& services) {
    // Capture the F18a audit sink by value (shared_ptr) so the handlers never
    // dangle on the registrar argument; null = audit disabled, never deref'd.
    auto op_logger = services.op_logger;
    (void)config;
    // GET /api/v1/memory?ns=...&user_id=...&status=...
    svr.Get("/api/v1/memory", WithAuth(auth, kPermRead,
        [&pool, &embedder, op_logger](const httplib::Request& req, httplib::Response& res,
                           const RequestContext& rc) {
        std::string ns = req.get_param_value("ns");
        if (ns.empty()) ns = req.get_param_value("namespace");
        if (ns.empty()) { WriteJsonError(res, Status::InvalidArgument("ns is required")); return; }
        // MEM05 L1: the requester may only list their own user_id (admin may query any).
        std::string user_id = req.get_param_value("user_id");
        if (!EnforceOwnUserId(rc, user_id, memory::Mem05Metrics::Action::kSearch)) {
            // Empty-result mask: do not reveal another user's memories exist.
            json resp; resp["memories"] = json::array(); resp["total"] = 0;
            WriteJsonResponse(res, 200, resp);
            return;
        }
        resource::NamespaceFacade facade(pool, ns);
        if (Status acq = facade.Acquire(); !acq.ok()) {
            WriteJsonError(res, Status::NotFound("Namespace '" + ns + "' not found")); return;
        }
        auto block_store = std::make_shared<memory::MemoryBlockAdapter>(
            facade.store(), &embedder, &facade.vec_index());
        memory::transparency::MemoryTransparency transparency(block_store, block_store, op_logger);
        memory::transparency::MemoryListFilter filter;
        filter.user_id = user_id;
        filter.include_invalidated = req.get_param_value("status") == "invalidated" ||
                                     req.get_param_value("include_invalidated") == "true";
        bool explain = req.get_param_value("explain") == "true";
        auto list = transparency.List(filter, user_id, explain);
        json resp;
        resp["memories"] = json::array();
        if (list.ok()) {
            for (const auto& it : list.value().memories) {
                json m;
                m["memory_id"] = it.memory_id;
                m["content"] = it.content;
                m["memory_type"] = it.memory_type;
                m["status"] = it.status;
                if (it.extracted_at) m["extracted_at"] = *it.extracted_at;
                if (it.revoked_at) m["revoked_at"] = *it.revoked_at;
                resp["memories"].push_back(m);
            }
            resp["total"] = list.value().total;
        } else {
            resp["total"] = 0;
        }
        WriteJsonResponse(res, 200, resp);
    }));

    // POST /api/v1/memory — create a memory fact (user self-service).
    svr.Post("/api/v1/memory", WithAuth(auth, kPermWrite,
        [&pool, &embedder, op_logger](const httplib::Request& req, httplib::Response& res,
                           const RequestContext& rc) {
        json body;
        try { body = json::parse(req.body); } catch (...) {
            WriteJsonError(res, Status::InvalidArgument("invalid JSON body")); return;
        }
        std::string ns = body.value("ns", body.value("namespace", ""));
        std::string content = body.value("content", "");
        if (ns.empty() || content.empty()) {
            WriteJsonError(res, Status::InvalidArgument("ns + content are required")); return;
        }
        // MEM05 L1: a non-admin may only create memories under its own user_id;
        // a spoofed body user_id is a cross-user write and is refused. 404 (not 403)
        // keeps the response shape uniform with the other masked routes and does not
        // confirm/deny anything about the target user.
        std::string user_id = body.value("user_id", "");
        if (!EnforceOwnUserId(rc, user_id, memory::Mem05Metrics::Action::kEdit)) {
            WriteJsonError(res, Status::NotFound("Cannot create memory for the requested user")); return;
        }
        resource::NamespaceFacade facade(pool, ns);
        if (Status acq = facade.Acquire(); !acq.ok()) {
            WriteJsonError(res, Status::NotFound("Namespace '" + ns + "' not found")); return;
        }
        auto block_store = std::make_shared<memory::MemoryBlockAdapter>(
            facade.store(), &embedder, &facade.vec_index());
        memory::transparency::MemoryTransparency transparency(block_store, block_store, op_logger);
        memory::transparency::MemoryCreateRequest creq;
        creq.user_id = user_id;
        creq.content = content;
        creq.memory_type = body.value("memory_type", "fact");
        auto r = transparency.Create(creq, user_id);
        if (!r.ok()) { WriteJsonError(res, r.status()); return; }
        json resp; resp["memory_id"] = r.value(); resp["status"] = "active";
        WriteJsonResponse(res, 201, resp);
    }));

    // PATCH /api/v1/memory/{id} — edit = new fact + invalidate old (MEM03-4).
    svr.Patch(R"(/api/v1/memory/([A-Za-z0-9_\-]+))", WithAuth(auth, kPermWrite,
        [&pool, &embedder, op_logger](const httplib::Request& req, httplib::Response& res,
                           const RequestContext& rc) {
        std::string mem_id = req.matches[1].str();
        json body;
        try { body = json::parse(req.body); } catch (...) {
            WriteJsonError(res, Status::InvalidArgument("invalid JSON body")); return;
        }
        std::string ns = body.value("ns", body.value("namespace", ""));
        std::string content = body.value("content", "");
        if (ns.empty() || content.empty()) {
            WriteJsonError(res, Status::InvalidArgument("ns + content are required")); return;
        }
        // MEM05 L1: a non-admin may only edit memories under its own user_id; a
        // spoofed body user_id would let A invalidate+rewrite B's memory → 404-mask.
        std::string user_id = body.value("user_id", "");
        if (!EnforceOwnUserId(rc, user_id, memory::Mem05Metrics::Action::kEdit)) {
            WriteJsonError(res, Status::NotFound("Memory not found: " + mem_id)); return;
        }
        resource::NamespaceFacade facade(pool, ns);
        if (Status acq = facade.Acquire(); !acq.ok()) {
            WriteJsonError(res, Status::NotFound("Namespace '" + ns + "' not found")); return;
        }
        auto block_store = std::make_shared<memory::MemoryBlockAdapter>(
            facade.store(), &embedder, &facade.vec_index());
        memory::transparency::MemoryTransparency transparency(block_store, block_store, op_logger);
        memory::transparency::MemoryEditRequest ereq;
        ereq.memory_id = mem_id;
        ereq.new_content = content;
        auto r = transparency.Edit(ereq, user_id);
        if (!r.ok()) { WriteJsonError(res, r.status()); return; }
        json resp;
        resp["new_memory_id"] = r.value().new_memory_id;
        resp["invalidated_memory_id"] = r.value().old_memory_id;
        WriteJsonResponse(res, 200, resp);
    }));

    // DELETE /api/v1/memory/{id} — soft-delete (status=invalidated, user_manual).
    svr.Delete(R"(/api/v1/memory/([A-Za-z0-9_\-]+))", WithAuth(auth, kPermWrite,
        [&pool, &embedder, op_logger](const httplib::Request& req, httplib::Response& res,
                           const RequestContext& rc) {
        std::string mem_id = req.matches[1].str();
        std::string ns = req.get_param_value("ns");
        if (ns.empty()) ns = req.get_param_value("namespace");
        if (ns.empty()) { WriteJsonError(res, Status::InvalidArgument("ns is required")); return; }
        // MEM05 L1: a non-admin may only soft-delete memories under its own user_id;
        // a spoofed user_id would let A invalidate B's memory → 404-mask.
        std::string user_id = req.get_param_value("user_id");
        if (!EnforceOwnUserId(rc, user_id, memory::Mem05Metrics::Action::kDelete)) {
            WriteJsonError(res, Status::NotFound("Memory not found: " + mem_id)); return;
        }
        resource::NamespaceFacade facade(pool, ns);
        if (Status acq = facade.Acquire(); !acq.ok()) {
            WriteJsonError(res, Status::NotFound("Namespace '" + ns + "' not found")); return;
        }
        auto block_store = std::make_shared<memory::MemoryBlockAdapter>(
            facade.store(), &embedder, &facade.vec_index());
        memory::transparency::MemoryTransparency transparency(block_store, block_store, op_logger);
        auto r = transparency.Delete(mem_id, user_id);
        if (!r.ok()) { WriteJsonError(res, r); return; }
        json resp; resp["block_id"] = mem_id; resp["status"] = "invalidated";
        WriteJsonResponse(res, 200, resp);
    }));
}

// ── M5: MEM04 Memory Immunity (opt-out) HTTP surface ─────────────────────────
// POST /api/v1/memory/session/{id}/opt-out         — opt a session out
// POST /api/v1/memory/session/{id}/opt-out/revoke  — admin revoke
// Sessions are NS-scoped, so the OptOutManager is built per-request over the NS façade
// (ns supplied in the body). The op-logger is left null here (the GEN-OperationLog hooks
// are recorded by the worker path); the HTTP path focuses on the state transition.
static void RegisterMemoryOptOutRoutes(httplib::Server& svr, ApiKeyAuth& auth,
                                       resource::INamespacePool& pool) {
    auto resolve_ns = [](const httplib::Request& req, std::string& ns) -> bool {
        json body = json::object();
        try { if (!req.body.empty()) body = json::parse(req.body); } catch (...) {}
        ns = body.value("ns", body.value("namespace", ""));
        return !ns.empty();
    };

    svr.Post(R"(/api/v1/memory/session/([A-Za-z0-9_\-]+)/opt-out/revoke)", WithAuth(auth, kPermAdmin,
        [&pool](const httplib::Request& req, httplib::Response& res, const RequestContext&) {
        std::string session_id = req.matches[1].str();
        json body = json::object();
        try { if (!req.body.empty()) body = json::parse(req.body); } catch (...) {}
        std::string ns = body.value("ns", body.value("namespace", ""));
        std::string reason = body.value("reason", "");
        if (ns.empty()) { WriteJsonError(res, Status::InvalidArgument("ns is required")); return; }
        if (reason.empty()) { WriteJsonError(res, Status::InvalidArgument("reason is required")); return; }
        resource::NamespaceFacade facade(pool, ns);
        if (Status acq = facade.Acquire(); !acq.ok()) {
            WriteJsonError(res, Status::NotFound("Namespace '" + ns + "' not found")); return;
        }
        memory::immunity::MemoryStoreOptOutAdapter opt_store(facade.memory());
        memory::immunity::OptOutManager opt_out(
            std::shared_ptr<memory::immunity::ISessionOptOutStore>(&opt_store, [](auto*) {}),
            nullptr, /*enabled=*/true);
        auto r = opt_out.OptOutRevoke(session_id, reason);
        if (!r.ok()) { WriteJsonError(res, r.status()); return; }
        json resp; resp["session_id"] = r.value().session_id; resp["revoked_at"] = r.value().revoked_at;
        WriteJsonResponse(res, 200, resp);
    }));

    svr.Post(R"(/api/v1/memory/session/([A-Za-z0-9_\-]+)/opt-out)", WithAuth(auth, kPermWrite,
        [&pool, resolve_ns](const httplib::Request& req, httplib::Response& res, const RequestContext& rc) {
        std::string session_id = req.matches[1].str();
        std::string ns;
        if (!resolve_ns(req, ns)) { WriteJsonError(res, Status::InvalidArgument("ns is required")); return; }
        json body = json::object();
        try { if (!req.body.empty()) body = json::parse(req.body); } catch (...) {}
        std::string reason = body.value("reason", "");
        resource::NamespaceFacade facade(pool, ns);
        if (Status acq = facade.Acquire(); !acq.ok()) {
            WriteJsonError(res, Status::NotFound("Namespace '" + ns + "' not found")); return;
        }
        // MEM05 L2: a non-admin may only opt out a session it owns; opting out
        // another user's session is 404-masked (anti-enumeration). (The revoke
        // sibling is kPermAdmin, so it is already restricted to admins.)
        if (!EnforceSessionOwner(rc, facade.memory(), session_id,
                                 memory::Mem05Metrics::Action::kSession)) {
            WriteJsonError(res, Status::NotFound("Session not found: " + session_id));
            return;
        }
        memory::immunity::MemoryStoreOptOutAdapter opt_store(facade.memory());
        memory::immunity::OptOutManager opt_out(
            std::shared_ptr<memory::immunity::ISessionOptOutStore>(&opt_store, [](auto*) {}),
            nullptr, /*enabled=*/true);
        auto r = opt_out.OptOut(session_id, memory::immunity::OptOutActor::kUserManual, reason);
        if (!r.ok()) {
            WriteJsonError(res, r.status());  // ALREADY_OPTED_OUT → 409 / NOT_FOUND → 404 via Status
            return;
        }
        json resp;
        resp["session_id"] = r.value().session_id;
        resp["opt_out_at"] = r.value().opt_out_at;
        resp["opted_out_by"] = r.value().opted_out_by;
        WriteJsonResponse(res, 200, resp);
    }));
}

void RegisterMemoryRoutes(
    httplib::Server& svr,
    ApiKeyAuth& auth,
    resource::INamespacePool& pool,
    SPCManager& spc_mgr,
    OnnxEmbedder& embedder,
    IntentClassifier& classifier,
    RRFFusion& fusion,
    const MemoryConfig& config,
    const MemoryServices& services)
{
    // POST /api/v1/memory/sessions — Create session
    svr.Post("/api/v1/memory/sessions", WithAuth(auth, kPermWrite,
        [&](const httplib::Request& req, httplib::Response& res, const RequestContext& rc) {
        json body;
        try {
            body = json::parse(req.body);
        } catch (...) {
            WriteJsonError(res, Status::InvalidArgument("Invalid JSON body"));
            return;
        }

        std::string ns_name = body.value("namespace", "");
        if (ns_name.empty()) {
            WriteJsonError(res, Status::InvalidArgument("namespace is required"));
            return;
        }

        // Per-request façade over the F05 pool (D-I5a Acquire/Release): the
        // destructor releases the bundle when this handler returns.
        resource::NamespaceFacade facade(pool, ns_name);
        Status acq = facade.Acquire();
        if (!acq.ok()) {
            WriteJsonError(res, Status::NotFound("Namespace '" + ns_name + "' not found: " + acq.message()));
            return;
        }
        MemoryStore* mem_store = &facade.memory();

        // Enforce max_sessions_per_namespace limit
        int64_t session_count = 0;
        mem_store->SessionCount(ns_name, &session_count);
        if (session_count >= config.max_sessions_per_namespace) {
            WriteJsonError(res, Status::InvalidArgument(
                "Max sessions per namespace limit reached (" +
                std::to_string(config.max_sessions_per_namespace) + ")"));
            return;
        }

        MemorySession session;
        session.session_id = body.value("session_id", "");  // Accept client-provided ID
        session.namespace_name = ns_name;
        // MEM05: a non-admin session owner is pinned to the authenticated principal
        // — a spoofed body user_id cannot create a session owned by another user
        // (closes the create-session owner-spoof griefing gap). Admin may set any
        // owner; the CE no-auth path (empty principal) falls back to body then
        // "default" so a session stays visible to its own creator.
        session.user_id = body.value("user_id", "");
        if (!rc.auth.is_admin() && !rc.auth.user_id.empty()) {
            session.user_id = rc.auth.user_id;
        } else if (session.user_id.empty()) {
            session.user_id = "default";
        }
        session.title = body.value("title", "");

        // Issue-12: a duplicate client-provided session_id must be a 409 with the
        // Agent-friendly envelope, not a raw 500 "UNIQUE constraint failed". Check
        // existence first (atomic enough for the single-writer memory.db).
        if (!session.session_id.empty()) {
            MemorySession existing;
            if (mem_store->SessionGet(session.session_id, existing).ok()) {
                WriteAgentError(res, 409, "CX_ERR_MEMORY_SESSION_EXISTS",
                                "session_id already exists", false, "permanent",
                                json{{"session_id", session.session_id}});
                return;
            }
        }

        auto s = mem_store->SessionCreate(session);
        if (!s.ok()) {
            // Belt-and-suspenders: a race that slipped past the pre-check surfaces as a
            // UNIQUE-constraint error → still map to 409 (not 500).
            if (s.message().find("UNIQUE") != std::string::npos ||
                s.message().find("already exists") != std::string::npos) {
                WriteAgentError(res, 409, "CX_ERR_MEMORY_SESSION_EXISTS",
                                "session_id already exists", false, "permanent",
                                json{{"session_id", session.session_id}});
                return;
            }
            WriteJsonError(res, s);
            return;
        }

        json resp;
        resp["session_id"] = session.session_id;
        resp["namespace"] = session.namespace_name;
        resp["user_id"] = session.user_id;
        resp["title"] = session.title;
        resp["created_at"] = session.created_at;
        WriteJsonResponse(res, 201, resp);
    }));

    // GET /api/v1/memory/sessions — List sessions
    svr.Get("/api/v1/memory/sessions", WithAuth(auth, kPermRead,
        [&](const httplib::Request& req, httplib::Response& res, const RequestContext& rc) {
        std::string ns_name = req.get_param_value("namespace");
        if (ns_name.empty()) {
            WriteJsonError(res, Status::InvalidArgument("namespace query param is required"));
            return;
        }

        resource::NamespaceFacade facade(pool, ns_name);
        Status acq = facade.Acquire();
        if (!acq.ok()) {
            WriteJsonError(res, Status::NotFound("Namespace '" + ns_name + "' not found: " + acq.message()));
            return;
        }

        int limit = 20;  // Design spec: default=20 (max: 100)
        int offset = 0;
        if (req.has_param("limit")) {
            try {
                limit = std::min(std::stoi(req.get_param_value("limit")), 100);
                if (limit < 1) limit = 20;
            } catch (const std::exception&) {
                WriteJsonError(res, Status::InvalidArgument("invalid 'limit' query parameter: must be an integer"));
                return;
            }
        }
        if (req.has_param("offset")) {
            try {
                offset = std::stoi(req.get_param_value("offset"));
                if (offset < 0) offset = 0;
            } catch (const std::exception&) {
                WriteJsonError(res, Status::InvalidArgument("invalid 'offset' query parameter: must be an integer"));
                return;
            }
        }

        MemoryStore* mem_store = &facade.memory();

        std::vector<MemorySession> sessions;
        auto s = mem_store->SessionList(ns_name, limit, offset, sessions);
        if (!s.ok()) {
            WriteJsonError(res, s);
            return;
        }

        // MEM05: list isolation — only return the requester's own sessions.
        // Authenticated non-admin: the principal (rc.auth.user_id) is authoritative,
        // so a spoofed ?user_id= cannot list another user's sessions. Admin / CE
        // no-auth fall back to the query-param self-identification ("default").
        // (Post-filter here keeps SessionList's store signature stable; pushing
        //  the user_id predicate into the SQL WHERE is a D3.5 store optimization.)
        std::string req_user_id = (!rc.auth.is_admin() && !rc.auth.user_id.empty())
                                      ? rc.auth.user_id
                                      : ResolveRequesterUserId(req);
        // §8.bis: list enforces isolation by post-filtering (no cross-user leak),
        // so the request itself is one isolation_check_total{result=pass,action=list}.
        memory::Mem05Metrics::Instance().RecordIsolationCheck(
            memory::Mem05Metrics::CheckResult::kPass,
            memory::Mem05Metrics::Action::kList);
        {
            std::vector<MemorySession> owned;
            owned.reserve(sessions.size());
            for (auto& sess : sessions) {
                if (sess.user_id == req_user_id) owned.push_back(std::move(sess));
            }
            sessions.swap(owned);
        }

        // Get total count for pagination (design spec: total_count + has_more)
        int64_t total_count = 0;
        mem_store->SessionCount(ns_name, &total_count);

        json resp;
        resp["sessions"] = json::array();
        for (const auto& sess : sessions) {
            json j;
            j["session_id"] = sess.session_id;
            j["namespace"] = sess.namespace_name;
            j["user_id"] = sess.user_id;
            j["title"] = sess.title;
            j["interaction_count"] = sess.interaction_count;
            j["created_at"] = sess.created_at;
            j["updated_at"] = sess.updated_at;
            resp["sessions"].push_back(j);
        }
        resp["total_count"] = total_count;
        resp["has_more"] = (offset + static_cast<int>(sessions.size())) < static_cast<int>(total_count);
        resp["limit"] = limit;
        resp["offset"] = offset;
        WriteJsonResponse(res, 200, resp);
    }));

    // GET /api/v1/memory/sessions/:session_id — Get session detail
    svr.Get(R"(/api/v1/memory/sessions/([A-Za-z0-9_\-]+))",
        WithAuth(auth, kPermRead,
        [&](const httplib::Request& req, httplib::Response& res, const RequestContext& rc) {
        std::string session_id = req.matches[1].str();

        std::string ns_name = req.get_param_value("namespace");
        if (ns_name.empty()) {
            WriteJsonError(res, Status::InvalidArgument("namespace query param is required"));
            return;
        }

        resource::NamespaceFacade facade(pool, ns_name);
        Status acq = facade.Acquire();
        if (!acq.ok()) {
            WriteJsonError(res, Status::NotFound("Namespace '" + ns_name + "' not found: " + acq.message()));
            return;
        }
        MemoryStore* mem_store = &facade.memory();

        MemorySession session;
        auto s = mem_store->SessionGet(session_id, session);
        if (!s.ok()) {
            WriteJsonError(res, s);
            return;
        }

        // MEM05: ownership check — a session belonging to another user must not
        // be readable. Return 404 (not 403) to avoid leaking existence (anti-enumeration,
        // design § 2.7 / § 4.5).
        //
        // When authenticated, the principal (rc.auth.user_id) is authoritative: a
        // non-admin must not read another user's session even if it passes the
        // victim's id in ?user_id= (that would be an IDOR). In CE no-auth mode the
        // principal is empty, so the requester self-identifies via the query param
        // (CE single-user compat) — the legacy param check below covers that case.
        if (!EnforceSessionOwner(rc, *mem_store, session_id,
                                 memory::Mem05Metrics::Action::kSessionAccess)) {
            WriteJsonError(res, Status::NotFound("Session not found: " + session_id));
            return;
        }
        // CE no-auth / admin: the authenticated principal is absent or unrestricted,
        // so the requester self-identifies via the query param. (For an
        // authenticated non-admin, EnforceSessionOwner above is the authoritative
        // gate and already passed; the param is not consulted.)
        if (rc.auth.is_admin() || rc.auth.user_id.empty()) {
            std::string req_user_id = ResolveRequesterUserId(req);
            const bool owned = (session.user_id == req_user_id);
            RecordIsolationDecision(memory::Mem05Metrics::Action::kSessionAccess, owned);
            if (!owned) {
                WriteJsonError(res, Status::NotFound("Session not found: " + session_id));
                return;
            }
        }

        std::vector<InteractionLog> interactions;
        mem_store->InteractionListBySession(session_id, interactions);

        json resp;
        resp["session"]["session_id"] = session.session_id;
        resp["session"]["namespace"] = session.namespace_name;
        resp["session"]["user_id"] = session.user_id;
        resp["session"]["title"] = session.title;
        resp["session"]["interaction_count"] = session.interaction_count;
        resp["session"]["created_at"] = session.created_at;
        resp["session"]["updated_at"] = session.updated_at;

        resp["interactions"] = json::array();
        for (const auto& log : interactions) {
            json j;
            j["id"] = log.id;
            j["role"] = log.role;
            j["content"] = log.content;
            if (!log.query_type.empty()) j["query_type"] = log.query_type;
            if (!log.status.empty()) j["status"] = log.status;
            if (log.latency_ms > 0) j["latency_ms"] = log.latency_ms;
            if (!log.metadata_json.empty()) {
                try {
                    j["metadata"] = json::parse(log.metadata_json);
                } catch (...) {
                    j["metadata"] = json::object();
                }
            }
            j["created_at"] = log.created_at;
            resp["interactions"].push_back(j);
        }

        WriteJsonResponse(res, 200, resp);
    }));

    // DELETE /api/v1/memory/sessions/:session_id — Delete session
    svr.Delete(R"(/api/v1/memory/sessions/([A-Za-z0-9_\-]+))",
        WithAuth(auth, kPermWrite,
        [&](const httplib::Request& req, httplib::Response& res, const RequestContext& rc) {
        std::string session_id = req.matches[1].str();
        std::string ns_name = req.get_param_value("namespace");
        if (ns_name.empty()) {
            WriteJsonError(res, Status::InvalidArgument("namespace query param is required"));
            return;
        }

        resource::NamespaceFacade facade(pool, ns_name);
        Status acq = facade.Acquire();
        if (!acq.ok()) {
            WriteJsonError(res, Status::NotFound("Namespace '" + ns_name + "' not found: " + acq.message()));
            return;
        }
        MemoryStore* mem_store = &facade.memory();

        // MEM05: verify ownership before deleting. Cross-user (or missing)
        // session -> 404, not leaking existence (anti-enumeration, design § 4.5).
        // Authenticated principal is authoritative (a non-admin cannot delete
        // another user's session by passing its id in ?user_id=); CE no-auth falls
        // back to the param self-identification below.
        if (!EnforceSessionOwner(rc, *mem_store, session_id,
                                 memory::Mem05Metrics::Action::kDelete)) {
            WriteJsonError(res, Status::NotFound("Session not found: " + session_id));
            return;
        }
        MemorySession session;
        auto get_s = mem_store->SessionGet(session_id, session);
        if (!get_s.ok()) {  // unknown session → not-found (no cross-user signal).
            WriteJsonError(res, Status::NotFound("Session not found: " + session_id));
            return;
        }
        // CE no-auth / admin self-identify via the query param (EnforceSessionOwner
        // is a no-op for them); an authenticated non-admin was already gated above.
        if (rc.auth.is_admin() || rc.auth.user_id.empty()) {
            std::string req_user_id = ResolveRequesterUserId(req);
            const bool owned = (session.user_id == req_user_id);
            RecordIsolationDecision(memory::Mem05Metrics::Action::kDelete, owned);
            if (!owned) {
                WriteJsonError(res, Status::NotFound("Session not found: " + session_id));
                return;
            }
        }

        int64_t count = 0;
        mem_store->InteractionCount(session_id, &count);

        auto s = mem_store->SessionDelete(session_id);
        if (!s.ok()) {
            WriteJsonError(res, s);
            return;
        }

        json resp;
        resp["deleted"] = true;
        resp["session_id"] = session_id;
        resp["interactions_deleted"] = count;
        WriteJsonResponse(res, 200, resp);
    }));

    // POST /api/v1/memory/sessions/:session_id/interactions — Write interaction
    // NOTE: capture the extraction pointer BY VALUE (services is a small bundle that may
    // be a default temporary at the call site; capturing it by reference would dangle).
    svr.Post(R"(/api/v1/memory/sessions/([A-Za-z0-9_\-]+)/interactions)",
        WithAuth(auth, kPermWrite,
        [&, extraction_svc = services.extraction](
            const httplib::Request& req, httplib::Response& res, const RequestContext& rc) {
        std::string session_id = req.matches[1].str();

        json body;
        try {
            body = json::parse(req.body);
        } catch (...) {
            WriteJsonError(res, Status::InvalidArgument("Invalid JSON body"));
            return;
        }

        std::string ns_name = body.value("namespace", "");
        if (ns_name.empty()) {
            WriteJsonError(res, Status::InvalidArgument("namespace is required"));
            return;
        }

        std::string query_text = body.value("query_text", "");
        std::string response_text = body.value("response_text", "");
        if (query_text.empty() || response_text.empty()) {
            WriteJsonError(res, Status::InvalidArgument("query_text and response_text are required"));
            return;
        }

        resource::NamespaceFacade facade(pool, ns_name);
        Status acq = facade.Acquire();
        if (!acq.ok()) {
            WriteJsonError(res, Status::NotFound("Namespace '" + ns_name + "' not found: " + acq.message()));
            return;
        }
        MemoryStore* mem_store = &facade.memory();

        // MEM05 L2: only the session's owner (or an admin) may append interactions;
        // writing into another user's session is 404-masked (anti-enumeration).
        if (!EnforceSessionOwner(rc, *mem_store, session_id,
                                 memory::Mem05Metrics::Action::kSession)) {
            WriteJsonError(res, Status::NotFound("Session not found: " + session_id));
            return;
        }

        // Enforce max_interactions_per_session limit
        int64_t interaction_count = 0;
        mem_store->InteractionCount(session_id, &interaction_count);
        if (interaction_count >= config.max_interactions_per_session) {
            WriteJsonError(res, Status::InvalidArgument(
                "Max interactions per session limit reached (" +
                std::to_string(config.max_interactions_per_session) + ")"));
            return;
        }

        MemoryWriter writer(*mem_store, spc_mgr, facade.store(), config);

        MemoryWriteRequest write_req;
        write_req.session_id = session_id;
        write_req.namespace_name = ns_name;
        write_req.user_id = body.value("user_id", "");
        write_req.query_text = query_text;
        write_req.query_type = body.value("query_type", "chat");
        write_req.response_text = response_text;
        write_req.response_status = body.value("response_status", "success");
        write_req.latency_ms = body.value("latency_ms", 0);
        write_req.result_source = body.value("result_source", "conversation");
        write_req.ttl_seconds = body.value("ttl_seconds", 0);
        if (body.contains("metadata")) {
            write_req.metadata_json = body["metadata"].dump();
        }

        auto s = writer.Write(write_req);
        if (!s.ok()) {
            WriteJsonError(res, s);
            return;
        }

        int64_t count = 0;
        mem_store->InteractionCount(session_id, &count);

        // [M1] Enqueue the turn for async MEM02 LLM extraction (the cortrix_log_interaction
        // trigger). The worker applies the MEM04 double-check (opt-out / remember) and runs
        // the NS-scoped extractor. Disabled (no LLM) => no-op, interaction_log only. The
        // user turn carries `remember`; absent defaults to true (extract).
        bool mem02_enqueued = false;
        if (extraction_svc && extraction_svc->enabled()) {
            InteractionLog turn;
            // Diagnostic identity for worker logs / dead-letter audit (the writer
            // keeps its own row id internally).
            turn.id = session_id + "/turn-" + std::to_string(count / 2 + 1);
            turn.session_id = session_id;
            turn.namespace_name = ns_name;
            turn.user_id = write_req.user_id;
            turn.role = "user";
            // Single-turn extraction unit = the query + its response (the window the LLM
            // sees). BuildWindowText renders "role: content"; we hand it the full exchange.
            turn.content = "user: " + query_text + "\nassistant: " + response_text;
            turn.query_type = write_req.query_type;
            turn.status = write_req.response_status;
            turn.remember = body.value("remember", true);
            mem02_enqueued = extraction_svc->Enqueue(turn);
        }

        json resp;
        resp["session_id"] = session_id;
        resp["turn"] = static_cast<int>(count / 2);
        resp["spc_enqueued"] = true;
        resp["memory_extraction_enqueued"] = mem02_enqueued;
        WriteJsonResponse(res, 201, resp);
    }));

    // POST /api/v1/memory/search — Memory semantic search
    // Design spec (6.5.3): Uses QueryPipeline for semantic vector search,
    // NOT SQL LIKE. Falls back to SQL LIKE if QueryPipeline is unavailable.
    svr.Post("/api/v1/memory/search", WithAuth(auth, kPermRead,
        [&](const httplib::Request& req, httplib::Response& res, const RequestContext& rc) {
        json body;
        try {
            body = json::parse(req.body);
        } catch (...) {
            WriteJsonError(res, Status::InvalidArgument("Invalid JSON body"));
            return;
        }

        std::string ns_name = body.value("namespace", "");
        if (ns_name.empty()) {
            WriteJsonError(res, Status::InvalidArgument("namespace is required"));
            return;
        }

        std::string query = body.value("query", "");
        if (query.empty()) {
            WriteJsonError(res, Status::InvalidArgument("query is required"));
            return;
        }

        resource::NamespaceFacade facade(pool, ns_name);
        Status acq = facade.Acquire();
        if (!acq.ok()) {
            WriteJsonError(res, Status::NotFound("Namespace '" + ns_name + "' not found: " + acq.message()));
            return;
        }

        MemorySearchRequest search_req;
        search_req.query = query;
        search_req.namespace_name = ns_name;
        search_req.top_k = body.value("top_k", 10);
        search_req.include_expired = body.value("include_expired", false);
        search_req.include_invalidated = body.value("include_invalidated", false);

        // MEM05: user_id is always required. With auth (P08) it comes from the
        // AuthContext (JWT/API Key); in CE no-auth mode it defaults to "default"
        // (design § CE single-user compatibility). Real AuthContext extraction is D3.5.
        // L1 IDOR guard: a non-admin requesting another user's memories via a
        // spoofed body user_id gets an empty result (no cross-user leak), the same
        // mask as GET /api/v1/memory list. EnforceOwnUserId records the isolation
        // decision (pass/violation); on a no-auth empty principal it resolves to
        // "default" — that fallback is also the cortrix_mem05_default_user_used
        // signal.
        std::string user_id = body.value("user_id", "");
        const bool user_id_was_supplied = !user_id.empty();
        if (!EnforceOwnUserId(rc, user_id, memory::Mem05Metrics::Action::kSearch)) {
            json resp; resp["results"] = json::array();
            resp["total_results"] = 0; resp["latency_ms"] = 0; resp["degraded"] = false;
            WriteJsonResponse(res, 200, resp);
            return;
        }
        if (!user_id_was_supplied && user_id == "default") {
            memory::Mem05Metrics::Instance().RecordDefaultUserUsed();  // CE no-auth fallback
        }
        search_req.user_id = user_id;

        // MEM05: scope is session or user only (kAll removed). Default = user.
        std::string scope_str = body.value("scope", "user");
        if (scope_str == "session") {
            search_req.scope = MemoryScope::kSession;
            search_req.session_id = body.value("session_id", "");
        } else {
            search_req.scope = MemoryScope::kUser;
        }

        auto valid = search_req.Validate();
        if (!valid.ok()) {
            WriteJsonError(res, valid);
            return;
        }

        MemoryStore* mem_store = &facade.memory();

        // Use semantic search via MemorySearcher + QueryPipeline (design spec 6.5.3)
        // Build a QueryPipeline for this namespace's stores
        VectorSearcher vec_searcher(facade.vec_index(), embedder);
        BM25Searcher bm25_searcher(facade.store());
        PostFilter post_filter(facade.store());
        DegradationManager deg_mgr;
        SqlExecutorStub sql_stub;
        QueryPipeline pipeline(vec_searcher, bm25_searcher, fusion,
                               classifier, post_filter, deg_mgr, sql_stub);

        // MEM01: inject the classified-decay scorer so /memory/search applies
        // event-decay ranking (fact/preference immune, invalidated filtered).
        // Decay params come from the global GUC (config.decay_*, design § 2.5;
        // D5 lock: V1.0 global-only). llm_available is logging-only and does not
        // affect scoring (memory_type "" and "event" decay identically, § 4.2);
        // RegisterMemoryRoutes does not receive the LLM config, so it is left at
        // its default — wiring it through is a follow-up (does not change ranking).
        MemoryDecayConfig decay_config;
        decay_config.lambda = config.decay_lambda;
        decay_config.min_score = config.decay_min_score;
        MemoryScorer scorer(decay_config);

        MemorySearcher searcher(pipeline, *mem_store, config, &scorer);
        MemorySearchResponse search_resp = searcher.Search(search_req);

        json resp;
        resp["results"] = json::array();
        for (const auto& item : search_resp.results) {
            json j;
            j["interaction_id"] = item.interaction_id;
            j["session_id"] = item.session_id;
            j["query_text"] = item.query_text;
            j["response_text"] = item.response_text;
            if (!item.query_type.empty()) j["query_type"] = item.query_type;
            // MEM02 fact/preference/event block fields (unified read pipeline §6.5.3):
            // present only on extracted-memory rows, absent on interaction rows.
            if (!item.content.empty()) j["content"] = item.content;
            if (!item.memory_type.empty()) j["memory_type"] = item.memory_type;
            if (!item.block_id.empty()) j["block_id"] = item.block_id;
            j["score"] = item.score;
            // MEM01 classified-decay transparency (design § 2.1 / S2 step 3):
            // score = raw RRF; final_score = raw * decay_factor (the ranking key).
            j["decay_factor"] = item.decay_factor;
            j["final_score"] = item.final_score;
            j["expired"] = item.expired;
            j["created_at"] = item.created_at;
            if (!item.metadata_json.empty()) j["metadata"] = item.metadata_json;
            resp["results"].push_back(j);
        }
        resp["total_results"] = search_resp.total_results;
        resp["latency_ms"] = search_resp.latency_ms;
        resp["degraded"] = search_resp.degraded;
        WriteJsonResponse(res, 200, resp);
    }));

    // POST /api/v1/memory/inject — Context injection
    // Design spec: Returns recent conversation context for a session,
    // assembled as "User: ...\nAssistant: ...\n\n..." format,
    // truncated to inject_max_tokens.
    svr.Post("/api/v1/memory/inject", WithAuth(auth, kPermRead,
        [&](const httplib::Request& req, httplib::Response& res, const RequestContext& rc) {
        json body;
        try {
            body = json::parse(req.body);
        } catch (...) {
            WriteJsonError(res, Status::InvalidArgument("Invalid JSON body"));
            return;
        }

        std::string ns_name = body.value("namespace", "");
        if (ns_name.empty()) {
            WriteJsonError(res, Status::InvalidArgument("namespace is required"));
            return;
        }

        std::string session_id = body.value("session_id", "");
        if (session_id.empty()) {
            WriteJsonError(res, Status::InvalidArgument("session_id is required"));
            return;
        }

        resource::NamespaceFacade facade(pool, ns_name);
        Status acq = facade.Acquire();
        if (!acq.ok()) {
            WriteJsonError(res, Status::NotFound("Namespace '" + ns_name + "' not found: " + acq.message()));
            return;
        }
        MemoryStore* mem_store = &facade.memory();

        // MEM05 L2: injecting a session's recent context returns its conversation
        // history — only the owner (or an admin) may read it. Cross-user → 404-mask
        // (anti-enumeration), consistent with GET /sessions/{id}.
        if (!EnforceSessionOwner(rc, *mem_store, session_id,
                                 memory::Mem05Metrics::Action::kSessionAccess)) {
            WriteJsonError(res, Status::NotFound("Session not found: " + session_id));
            return;
        }

        MemoryInjector injector(*mem_store, config);
        InjectedContext ctx = injector.GetRecentContext(session_id);

        json resp;
        resp["session_id"] = session_id;
        resp["context_text"] = ctx.context_text;
        resp["turn_count"] = ctx.turn_count;
        resp["token_count_approx"] = ctx.token_count_approx;
        WriteJsonResponse(res, 200, resp);
    }));

    RegisterMemoryExtractRoutes(svr, auth, pool, embedder, config, services.extraction);
    RegisterMemoryTransparencyRoutes(svr, auth, pool, embedder, config, services);
    RegisterMemoryOptOutRoutes(svr, auth, pool);
}

}  // namespace cortrix
