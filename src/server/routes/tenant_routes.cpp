#include <cstdint>
#include "cortrix/server/routes/tenant_routes.h"

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cortrix/agent_friendly/error.h"
#include "cortrix/auth/auth_middleware.h"
#include "cortrix/server/http_server.h"
#include "cortrix/tenant/permission_service.h"
#include "cortrix/tenant/quota_service.h"
#include "cortrix/tenant/tenant_error.h"
#include "cortrix/tenant/tenant_service.h"

namespace cortrix {

namespace {

using tenant::NsAclRole;
using tenant::TenantErrorCode;
using tenant::TenantRole;

// All tenant error codes, so we can recover the identity from the CX token a
// TenantStatus() message carries ("CX_ERR_X: detail" -- see tenant_error.h).
const std::vector<TenantErrorCode>& AllTenantErrorCodes() {
    static const std::vector<TenantErrorCode> kAll = {
        TenantErrorCode::kTenantNotFound, TenantErrorCode::kTenantAlreadyExists,
        TenantErrorCode::kTenantEmailDuplicate, TenantErrorCode::kTenantNameDuplicate,
        TenantErrorCode::kTenantDeleteHasNamespaces, TenantErrorCode::kTenantDeleteHasMembers,
        TenantErrorCode::kTenantTransferTargetNotMember, TenantErrorCode::kTenantLastOwnerRemoval,
        TenantErrorCode::kMemberAlreadyExists, TenantErrorCode::kMemberNotFound,
        TenantErrorCode::kMemberRoleInvalid, TenantErrorCode::kNsUnauthorized,
        TenantErrorCode::kNsNotFound, TenantErrorCode::kAclGrantToOwnerTenant,
        TenantErrorCode::kAclDuplicateGrant, TenantErrorCode::kAclGrantInvalidRole,
        TenantErrorCode::kAclNotFound, TenantErrorCode::kPermissionOwnerOnly,
        TenantErrorCode::kTenantQuotaExceeded, TenantErrorCode::kTenantQuotaNotFound,
        TenantErrorCode::kTenantTransactionFailed, TenantErrorCode::kAdminKeyRequired,
        TenantErrorCode::kCallerNotMember, TenantErrorCode::kCallerRoleInsufficient,
    };
    return kAll;
}

/// Recover the TenantErrorCode whose CX string prefixes `status.message()`.
std::optional<TenantErrorCode> CodeFromStatus(const Status& s) {
    const std::string& msg = s.message();
    for (auto code : AllTenantErrorCodes()) {
        const std::string token = tenant::TenantErrorCodeString(code);
        if (msg.rfind(token, 0) == 0) return code;  // prefix match
    }
    return std::nullopt;
}

/// HTTP status for a tenant error code (the categories map to the standard codes).
int HttpForCode(TenantErrorCode code) {
    switch (tenant::TenantErrorToStatusCode(code)) {
        case StatusCode::kNotFound:        return 404;
        case StatusCode::kAlreadyExists:   return 409;
        case StatusCode::kPermissionDenied: return 403;
        case StatusCode::kUnauthenticated: return 401;
        case StatusCode::kInvalidArgument: return 400;
        case StatusCode::kUnavailable:     return 503;
        default:                           return 500;
    }
}

/// Write the sec 7 Agent-friendly error envelope. `structured_data` carries the sec 7
/// required keys for the code; the registry fills category/retryable/retry_after.
void WriteTenantError(httplib::Response& res, TenantErrorCode code,
                      nlohmann::json structured_data, const std::string& message,
                      const std::string& request_id) {
    auto err = tenant::MakeTenantError(code, std::move(structured_data), message);
    nlohmann::json body;
    body["error"] = agent_friendly::ToJson(err);
    if (!request_id.empty()) {
        body["error"]["request_id"] = request_id;
        res.set_header("X-Request-Id", request_id);
    }
    res.set_content(body.dump(), "application/json");
    res.status = HttpForCode(code);
}

/// Bridge a service Status to the Agent-friendly envelope by recovering the
/// embedded CX identity (falls back to a generic transaction-failed body).
void WriteStatusError(httplib::Response& res, const Status& s, nlohmann::json structured_data,
                      const std::string& request_id) {
    TenantErrorCode code = CodeFromStatus(s).value_or(TenantErrorCode::kTenantTransactionFailed);
    // Strip the "CX_ERR_X: " prefix so the human message is clean.
    std::string msg = s.message();
    const std::string token = tenant::TenantErrorCodeString(code);
    if (msg.rfind(token + ": ", 0) == 0) {
        msg = msg.substr(token.size() + 2);
    } else if (msg == token) {
        msg.clear();
    }
    WriteTenantError(res, code, std::move(structured_data), msg, request_id);
}

/// Parse a request JSON body; returns std::nullopt on malformed input.
std::optional<nlohmann::json> ParseBody(const httplib::Request& req) {
    if (req.body.empty()) return nlohmann::json::object();
    auto j = nlohmann::json::parse(req.body, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded()) return std::nullopt;
    return j;
}

nlohmann::json TenantToJson(const tenant::Tenant& t) {
    return {{"tenant_id", t.tenant_id},
            {"name", t.name},
            {"type", tenant::ToString(t.type)},
            {"dedup_scope", t.dedup_scope},
            {"created_at", t.created_at}};
}

nlohmann::json MemberToJson(const tenant::Member& m) {
    return {{"user_id", m.user_id},
            {"email", m.email},
            {"role", tenant::ToString(m.role)},
            {"joined_at", m.joined_at}};
}

nlohmann::json AclToJson(const tenant::AclEntry& e) {
    return {{"grantee_tenant_id", e.grantee_tenant_id},
            {"grantee_user_id", e.grantee_user_id},
            {"role", tenant::ToString(e.role)},
            {"granted_at", e.granted_at},
            {"granted_by_user_id", e.granted_by_user_id}};
}

}  // namespace

void RegisterTenantRoutes(httplib::Server& server, tenant::TenantService& tenant_svc,
                          tenant::PermissionService& perm_svc, tenant::QuotaService& quota_svc,
                          ApiKeyAuth& auth) {
    // ===================== Tenant CRUD =====================

    // POST /api/v1/tenants -- CreateOrganization (admin-only, sec 4.5).
    server.Post("/api/v1/tenants",
        WithAuth(auth, kPermAdmin,
            [&tenant_svc](const httplib::Request& req, httplib::Response& res,
                          const RequestContext& rctx) {
                if (!rctx.auth.is_admin()) {
                    WriteTenantError(res, TenantErrorCode::kAdminKeyRequired,
                                     {{"endpoint", "/api/v1/tenants"}, {"required_role", "owner"}},
                                     "admin API key required", rctx.request_id);
                    return;
                }
                auto body = ParseBody(req);
                if (!body || !body->contains("name") || !(*body)["name"].is_string()) {
                    WriteTenantError(res, TenantErrorCode::kMemberRoleInvalid,
                                     {{"role", "n/a"}, {"valid_roles", nlohmann::json::array()}},
                                     "missing 'name'", rctx.request_id);
                    return;
                }
                auto result = tenant_svc.CreateOrganization((*body)["name"].get<std::string>(),
                                                            rctx.auth);
                if (!result.ok()) {
                    WriteStatusError(res, result.status(),
                                     {{"name", (*body)["name"]}, {"tenant_id", ""}}, rctx.request_id);
                    return;
                }
                WriteJsonResponse(res, 201, {{"tenant", TenantToJson(*result)}}, rctx.request_id);
            }));

    // GET /api/v1/tenants -- admin list-all (sec 4.5). Standalone: list-all wiring is a
    // thin admin convenience; returns admin-gated empty/forbidden per is_admin.
    server.Get("/api/v1/tenants",
        WithAuth(auth, kPermAdmin,
            [](const httplib::Request&, httplib::Response& res, const RequestContext& rctx) {
                if (!rctx.auth.is_admin()) {
                    WriteTenantError(res, TenantErrorCode::kAdminKeyRequired,
                                     {{"endpoint", "/api/v1/tenants"}, {"required_role", "owner"}},
                                     "admin API key required", rctx.request_id);
                    return;
                }
                // The admin list-all enumeration is a Cloud-V1 admin surface; V1.0
                // OSS returns the structurally-correct empty envelope (single
                // default_tenant deployments do not enumerate). integration wires the
                // full SELECT once admin list semantics are integrated.
                WriteJsonResponse(res, 200, {{"tenants", nlohmann::json::array()}}, rctx.request_id);
            }));

    // GET /api/v1/tenants/{tenant_id} -- Get (admin or member, sec 4.5).
    server.Get(R"(/api/v1/tenants/([^/]+))",
        WithAuth(auth, kPermRead,
            [&tenant_svc](const httplib::Request& req, httplib::Response& res,
                          const RequestContext& rctx) {
                std::string tid = req.matches.size() > 1 ? req.matches[1].str() : "";
                auto result = tenant_svc.Get(tid, rctx.auth);
                if (!result.ok()) {
                    WriteStatusError(res, result.status(),
                                     {{"tenant_id", tid}, {"user_id", rctx.auth.user_id}},
                                     rctx.request_id);
                    return;
                }
                WriteJsonResponse(res, 200, {{"tenant", TenantToJson(*result)}}, rctx.request_id);
            }));

    // PATCH /api/v1/tenants/{tenant_id} -- UpdateName (admin, sec 4.5).
    server.Patch(R"(/api/v1/tenants/([^/]+))",
        WithAuth(auth, kPermAdmin,
            [&tenant_svc](const httplib::Request& req, httplib::Response& res,
                          const RequestContext& rctx) {
                std::string tid = req.matches.size() > 1 ? req.matches[1].str() : "";
                auto body = ParseBody(req);
                if (!body || !body->contains("name") || !(*body)["name"].is_string()) {
                    WriteTenantError(res, TenantErrorCode::kMemberRoleInvalid,
                                     {{"role", "n/a"}, {"valid_roles", nlohmann::json::array()}},
                                     "missing 'name'", rctx.request_id);
                    return;
                }
                auto st = tenant_svc.UpdateName(tid, (*body)["name"].get<std::string>(), rctx.auth);
                if (!st.ok()) {
                    WriteStatusError(res, st, {{"tenant_id", tid}, {"user_id", rctx.auth.user_id}},
                                     rctx.request_id);
                    return;
                }
                WriteJsonResponse(res, 200, {{"ok", true}}, rctx.request_id);
            }));

    // DELETE /api/v1/tenants/{tenant_id} -- Delete (admin, sec 4.5).
    server.Delete(R"(/api/v1/tenants/([^/]+))",
        WithAuth(auth, kPermAdmin,
            [&tenant_svc](const httplib::Request& req, httplib::Response& res,
                          const RequestContext& rctx) {
                std::string tid = req.matches.size() > 1 ? req.matches[1].str() : "";
                auto st = tenant_svc.Delete(tid, rctx.auth);
                if (!st.ok()) {
                    WriteStatusError(res, st,
                                     {{"tenant_id", tid}, {"ns_count", 0},
                                      {"ns_ids", nlohmann::json::array()}, {"member_count", 0},
                                      {"user_id", rctx.auth.user_id}},
                                     rctx.request_id);
                    return;
                }
                WriteJsonResponse(res, 200, {{"ok", true}}, rctx.request_id);
            }));

    // ===================== Member management =====================

    // POST /api/v1/tenants/{tenant_id}/members -- AddMember (admin, sec 4.5).
    server.Post(R"(/api/v1/tenants/([^/]+)/members)",
        WithAuth(auth, kPermAdmin,
            [&tenant_svc](const httplib::Request& req, httplib::Response& res,
                          const RequestContext& rctx) {
                std::string tid = req.matches.size() > 1 ? req.matches[1].str() : "";
                auto body = ParseBody(req);
                if (!body || !body->contains("user_id") || !(*body)["user_id"].is_string() ||
                    !body->contains("role") || !(*body)["role"].is_string()) {
                    WriteTenantError(res, TenantErrorCode::kMemberRoleInvalid,
                                     {{"role", "n/a"},
                                      {"valid_roles", {"owner", "admin", "member", "viewer"}}},
                                     "missing 'user_id'/'role'", rctx.request_id);
                    return;
                }
                auto role = tenant::ParseTenantRole((*body)["role"].get<std::string>());
                if (!role) {
                    WriteTenantError(res, TenantErrorCode::kMemberRoleInvalid,
                                     {{"role", (*body)["role"]},
                                      {"valid_roles", {"owner", "admin", "member", "viewer"}}},
                                     "invalid role", rctx.request_id);
                    return;
                }
                auto st = tenant_svc.AddMember(tid, (*body)["user_id"].get<std::string>(), *role,
                                               rctx.auth);
                if (!st.ok()) {
                    WriteStatusError(res, st,
                                     {{"tenant_id", tid}, {"user_id", (*body)["user_id"]},
                                      {"current_role", ""}},
                                     rctx.request_id);
                    return;
                }
                WriteJsonResponse(res, 201, {{"ok", true}}, rctx.request_id);
            }));

    // GET /api/v1/tenants/{tenant_id}/members -- ListMembers (admin or member, sec 4.5).
    server.Get(R"(/api/v1/tenants/([^/]+)/members)",
        WithAuth(auth, kPermRead,
            [&tenant_svc](const httplib::Request& req, httplib::Response& res,
                          const RequestContext& rctx) {
                std::string tid = req.matches.size() > 1 ? req.matches[1].str() : "";
                auto result = tenant_svc.ListMembers(tid, rctx.auth);
                if (!result.ok()) {
                    WriteStatusError(res, result.status(),
                                     {{"tenant_id", tid}, {"user_id", rctx.auth.user_id}},
                                     rctx.request_id);
                    return;
                }
                nlohmann::json arr = nlohmann::json::array();
                for (const auto& m : *result) arr.push_back(MemberToJson(m));
                WriteJsonResponse(res, 200, {{"members", arr}}, rctx.request_id);
            }));

    // DELETE /api/v1/tenants/{tenant_id}/members/{user_id} -- RemoveMember (admin).
    server.Delete(R"(/api/v1/tenants/([^/]+)/members/([^/]+))",
        WithAuth(auth, kPermAdmin,
            [&tenant_svc](const httplib::Request& req, httplib::Response& res,
                          const RequestContext& rctx) {
                std::string tid = req.matches.size() > 1 ? req.matches[1].str() : "";
                std::string uid = req.matches.size() > 2 ? req.matches[2].str() : "";
                auto st = tenant_svc.RemoveMember(tid, uid, rctx.auth);
                if (!st.ok()) {
                    WriteStatusError(res, st, {{"tenant_id", tid}, {"user_id", uid}},
                                     rctx.request_id);
                    return;
                }
                WriteJsonResponse(res, 200, {{"ok", true}}, rctx.request_id);
            }));

    // PATCH /api/v1/tenants/{tenant_id}/members/{user_id} -- UpdateMemberRole (admin).
    server.Patch(R"(/api/v1/tenants/([^/]+)/members/([^/]+))",
        WithAuth(auth, kPermAdmin,
            [&tenant_svc](const httplib::Request& req, httplib::Response& res,
                          const RequestContext& rctx) {
                std::string tid = req.matches.size() > 1 ? req.matches[1].str() : "";
                std::string uid = req.matches.size() > 2 ? req.matches[2].str() : "";
                auto body = ParseBody(req);
                if (!body || !body->contains("role") || !(*body)["role"].is_string()) {
                    WriteTenantError(res, TenantErrorCode::kMemberRoleInvalid,
                                     {{"role", "n/a"},
                                      {"valid_roles", {"owner", "admin", "member", "viewer"}}},
                                     "missing 'role'", rctx.request_id);
                    return;
                }
                auto role = tenant::ParseTenantRole((*body)["role"].get<std::string>());
                if (!role) {
                    WriteTenantError(res, TenantErrorCode::kMemberRoleInvalid,
                                     {{"role", (*body)["role"]},
                                      {"valid_roles", {"owner", "admin", "member", "viewer"}}},
                                     "invalid role", rctx.request_id);
                    return;
                }
                auto st = tenant_svc.UpdateMemberRole(tid, uid, *role, rctx.auth);
                if (!st.ok()) {
                    WriteStatusError(res, st, {{"tenant_id", tid}, {"user_id", uid}},
                                     rctx.request_id);
                    return;
                }
                WriteJsonResponse(res, 200, {{"ok", true}}, rctx.request_id);
            }));

    // POST /api/v1/tenants/{tenant_id}/transfer-ownership -- TransferOwnership (admin).
    server.Post(R"(/api/v1/tenants/([^/]+)/transfer-ownership)",
        WithAuth(auth, kPermAdmin,
            [&tenant_svc](const httplib::Request& req, httplib::Response& res,
                          const RequestContext& rctx) {
                std::string tid = req.matches.size() > 1 ? req.matches[1].str() : "";
                auto body = ParseBody(req);
                if (!body || !body->contains("new_owner_user_id") ||
                    !(*body)["new_owner_user_id"].is_string()) {
                    WriteTenantError(res, TenantErrorCode::kTenantTransferTargetNotMember,
                                     {{"tenant_id", tid}, {"target_user_id", ""}},
                                     "missing 'new_owner_user_id'", rctx.request_id);
                    return;
                }
                auto st = tenant_svc.TransferOwnership(
                    tid, (*body)["new_owner_user_id"].get<std::string>(), rctx.auth);
                if (!st.ok()) {
                    WriteStatusError(res, st,
                                     {{"tenant_id", tid},
                                      {"target_user_id", (*body)["new_owner_user_id"]}},
                                     rctx.request_id);
                    return;
                }
                WriteJsonResponse(res, 200, {{"ok", true}}, rctx.request_id);
            }));

    // ===================== Namespace ACL =====================

    // GET /api/v1/namespaces/{ns_id}/acl -- ListAcl (owner / ACL admin).
    server.Get(R"(/api/v1/namespaces/([^/]+)/acl)",
        WithAuth(auth, kPermRead,
            [&perm_svc](const httplib::Request& req, httplib::Response& res,
                        const RequestContext& rctx) {
                std::string ns = req.matches.size() > 1 ? req.matches[1].str() : "";
                auto result = perm_svc.ListAcl(ns, rctx.auth);
                if (!result.ok()) {
                    WriteStatusError(res, result.status(),
                                     {{"ns_id", ns}, {"tenant_id", rctx.auth.tenant_id},
                                      {"user_id", rctx.auth.user_id}, {"required_action", "admin"}},
                                     rctx.request_id);
                    return;
                }
                nlohmann::json arr = nlohmann::json::array();
                for (const auto& e : *result) arr.push_back(AclToJson(e));
                WriteJsonResponse(res, 200, {{"acl", arr}}, rctx.request_id);
            }));

    // POST /api/v1/namespaces/{ns_id}/acl -- GrantAcl (owner / ACL admin).
    server.Post(R"(/api/v1/namespaces/([^/]+)/acl)",
        WithAuth(auth, kPermWrite,
            [&perm_svc](const httplib::Request& req, httplib::Response& res,
                        const RequestContext& rctx) {
                std::string ns = req.matches.size() > 1 ? req.matches[1].str() : "";
                auto body = ParseBody(req);
                if (!body || !body->contains("grantee_tenant_id") ||
                    !(*body)["grantee_tenant_id"].is_string() || !body->contains("role") ||
                    !(*body)["role"].is_string()) {
                    WriteTenantError(res, TenantErrorCode::kAclGrantInvalidRole,
                                     {{"role", "n/a"}, {"valid_roles", {"viewer", "editor", "admin"}}},
                                     "missing 'grantee_tenant_id'/'role'", rctx.request_id);
                    return;
                }
                auto role = tenant::ParseNsAclRole((*body)["role"].get<std::string>());
                if (!role) {
                    WriteTenantError(res, TenantErrorCode::kAclGrantInvalidRole,
                                     {{"role", (*body)["role"]},
                                      {"valid_roles", {"viewer", "editor", "admin"}}},
                                     "invalid ACL role", rctx.request_id);
                    return;
                }
                std::string gu = (body->contains("grantee_user_id") &&
                                  (*body)["grantee_user_id"].is_string())
                                     ? (*body)["grantee_user_id"].get<std::string>()
                                     : "";
                auto st = perm_svc.GrantAcl(ns, (*body)["grantee_tenant_id"].get<std::string>(), gu,
                                            *role, rctx.auth);
                if (!st.ok()) {
                    WriteStatusError(res, st,
                                     {{"ns_id", ns}, {"tenant_id", (*body)["grantee_tenant_id"]},
                                      {"grantee_tenant_id", (*body)["grantee_tenant_id"]},
                                      {"grantee_user_id", gu}, {"existing_role", ""},
                                      {"user_id", rctx.auth.user_id}, {"required_action", "admin"}},
                                     rctx.request_id);
                    return;
                }
                WriteJsonResponse(res, 201, {{"ok", true}}, rctx.request_id);
            }));

    // DELETE /api/v1/namespaces/{ns_id}/acl/{grantee_tenant_id} -- RevokeAcl.
    server.Delete(R"(/api/v1/namespaces/([^/]+)/acl/([^/]+))",
        WithAuth(auth, kPermWrite,
            [&perm_svc](const httplib::Request& req, httplib::Response& res,
                        const RequestContext& rctx) {
                std::string ns = req.matches.size() > 1 ? req.matches[1].str() : "";
                std::string gt = req.matches.size() > 2 ? req.matches[2].str() : "";
                // Optional grantee_user_id via query param (tenant-level grant when absent).
                std::string gu;
                if (req.has_param("grantee_user_id")) gu = req.get_param_value("grantee_user_id");
                auto st = perm_svc.RevokeAcl(ns, gt, gu, rctx.auth);
                if (!st.ok()) {
                    WriteStatusError(res, st,
                                     {{"ns_id", ns}, {"grantee_tenant_id", gt},
                                      {"grantee_user_id", gu}, {"tenant_id", rctx.auth.tenant_id},
                                      {"user_id", rctx.auth.user_id}, {"required_action", "admin"}},
                                     rctx.request_id);
                    return;
                }
                WriteJsonResponse(res, 200, {{"ok", true}}, rctx.request_id);
            }));

    // ===================== Quota =====================

    // GET /api/v1/tenants/{tenant_id}/quota -- GetQuota (member, sec 4.5).
    server.Get(R"(/api/v1/tenants/([^/]+)/quota)",
        WithAuth(auth, kPermRead,
            [&quota_svc](const httplib::Request& req, httplib::Response& res,
                         const RequestContext& rctx) {
                std::string tid = req.matches.size() > 1 ? req.matches[1].str() : "";
                auto result = quota_svc.GetQuota(tid, rctx.auth);
                if (!result.ok()) {
                    WriteStatusError(res, result.status(),
                                     {{"tenant_id", tid}, {"quota_type", ""}}, rctx.request_id);
                    return;
                }
                nlohmann::json limits = nlohmann::json::object();
                nlohmann::json usage = nlohmann::json::object();
                for (const auto& [type, lim] : result->limits) {
                    limits[tenant::ToString(type)] = lim.IsUnlimited() ? -1 : lim.limit;
                }
                for (const auto& [type, u] : result->usage) {
                    usage[tenant::ToString(type)] = u;
                }
                WriteJsonResponse(res, 200,
                                  {{"tenant_id", tid}, {"limits", limits}, {"usage", usage}},
                                  rctx.request_id);
            }));

    // PATCH /api/v1/admin/tenants/{tenant_id}/quota -- UpdateQuotaOverride (admin, sec 4.5).
    server.Patch(R"(/api/v1/admin/tenants/([^/]+)/quota)",
        WithAuth(auth, kPermAdmin,
            [&quota_svc](const httplib::Request& req, httplib::Response& res,
                         const RequestContext& rctx) {
                std::string tid = req.matches.size() > 1 ? req.matches[1].str() : "";
                if (!rctx.auth.is_admin()) {
                    WriteTenantError(res, TenantErrorCode::kAdminKeyRequired,
                                     {{"endpoint", "/api/v1/admin/tenants/{id}/quota"},
                                      {"required_role", "owner"}},
                                     "admin API key required", rctx.request_id);
                    return;
                }
                auto body = ParseBody(req);
                if (!body || !body->contains("quota_type") || !(*body)["quota_type"].is_string() ||
                    !body->contains("limit") || !(*body)["limit"].is_number_integer()) {
                    WriteTenantError(res, TenantErrorCode::kTenantQuotaNotFound,
                                     {{"tenant_id", tid}, {"quota_type", ""}},
                                     "missing 'quota_type'/'limit'", rctx.request_id);
                    return;
                }
                // Map the quota_type token (reuse the QuotaType ToString tokens).
                static const std::pair<const char*, tenant::QuotaType> kMap[] = {
                    {"max_namespaces", tenant::QuotaType::MAX_NAMESPACES},
                    {"max_documents_per_ns", tenant::QuotaType::MAX_DOCUMENTS_PER_NS},
                    {"max_storage_bytes", tenant::QuotaType::MAX_STORAGE_BYTES},
                    {"max_qps", tenant::QuotaType::MAX_QPS},
                    {"max_members_per_tenant", tenant::QuotaType::MAX_MEMBERS_PER_TENANT}};
                std::optional<tenant::QuotaType> qt;
                std::string qt_token = (*body)["quota_type"].get<std::string>();
                for (const auto& [tok, val] : kMap) {
                    if (qt_token == tok) { qt = val; break; }
                }
                if (!qt) {
                    WriteTenantError(res, TenantErrorCode::kTenantQuotaNotFound,
                                     {{"tenant_id", tid}, {"quota_type", qt_token}},
                                     "unknown quota_type", rctx.request_id);
                    return;
                }
                tenant::QuotaLimit lim{(*body)["limit"].get<int64_t>()};
                auto st = quota_svc.UpdateQuotaOverride(tid, *qt, lim, rctx.auth);
                if (!st.ok()) {
                    WriteStatusError(res, st, {{"tenant_id", tid}, {"quota_type", qt_token}},
                                     rctx.request_id);
                    return;
                }
                WriteJsonResponse(res, 200, {{"ok", true}}, rctx.request_id);
            }));

    // NOTE: POST /api/v1/auth/switch-tenant is deliberately NOT registered (topic 5):
    // an unregistered route -> HTTP 404, the Agent-friendly "not available" signal
    // (BACKLOG TD-AUTH-SWITCH-TENANT-ENDPOINT). Do not add it here.
}

}  // namespace cortrix
