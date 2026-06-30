#include <cstdint>
#include "cortrix/server/http_server.h"
#include "cortrix/auth/auth_middleware.h"
#include "cortrix/agent_friendly/error.h"       // GEN-Agent error category enum
#include "cortrix/catalog/i_ns_router.h"        // F12 INSRouter (F13 create path)
#include "cortrix/catalog/catalog_types.h"      // NSMetadata
#include "cortrix/resource/namespace_facade.h"  // per-request façade over F05 pool
#include "cortrix/logging/logging.h"
#include "cortrix/common/version.h"                       // [P5] version SoT
#include "cortrix/observability/operation_logger.h"       // F18a ns_* operation_log
#include "cortrix/observability/operation_log_emitter.h"  // §9.1 MakeEngineEntry / EmitSite

#include <cstdlib>
#include <cctype>
#include <algorithm>
#include <random>
#include <iomanip>
#include <sstream>
#include <fstream>
#include <chrono>
#include <ctime>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

#include <openssl/rand.h>
#include <yaml-cpp/yaml.h>

namespace cortrix {

// GenerateRequestId: 16 random bytes -> 32-char hex string
std::string GenerateRequestId() {
    unsigned char buf[16];
    RAND_bytes(buf, sizeof(buf));

    std::ostringstream ss;
    ss << std::hex << std::setfill('0');
    for (int i = 0; i < 16; ++i) {
        ss << std::setw(2) << static_cast<int>(buf[i]);
    }
    return ss.str();
}

namespace {

using agent_friendly::ErrorCategory;

// CX_ERR_* -> {category, retryable} for the codes ERROR_CODE_SDK_MAP.md sec.3
// (the L2 high-frequency subclass set) annotates explicitly. This is the doc SoT
// the coarse StatusCode cannot reconstruct: e.g. quota / timeout codes coarsen
// onto kPermissionDenied / kInvalidArgument and would otherwise mislabel as
// auth / permanent. Scoped to sec.3's curated L2 set on purpose - the broader
// in-repo registries include shared codes (e.g. CX_ERR_INTERNAL_ERROR carries
// conflicting category across auth vs catalog) and divergent spellings, so a
// global string map is not 1:1; sec.3 is conflict-free and authoritative for
// these high-frequency identities. Anything not listed falls back to the
// StatusCode approximation below.
const std::unordered_map<std::string, std::pair<ErrorCategory, bool>>& Sdk3Map() {
    static const std::unordered_map<std::string, std::pair<ErrorCategory, bool>> kMap = {
        // sec.3.2 Auth (P08, 11 codes)
        {"CX_ERR_AUTH_EMAIL_ALREADY_EXISTS", {ErrorCategory::kPermanent, false}},
        {"CX_ERR_AUTH_INVALID_CREDENTIALS", {ErrorCategory::kAuth, false}},
        {"CX_ERR_AUTH_INVALID_RESET_CODE", {ErrorCategory::kPermanent, false}},
        {"CX_ERR_AUTH_TOKEN_EXPIRED", {ErrorCategory::kAuth, false}},
        {"CX_ERR_AUTH_BOOTSTRAP_TOKEN_INVALID", {ErrorCategory::kAuth, false}},
        {"CX_ERR_AUTH_INVALID_API_KEY", {ErrorCategory::kAuth, false}},
        {"CX_ERR_AUTH_ADMIN_REQUIRED", {ErrorCategory::kAuth, false}},
        {"CX_ERR_AUTH_TOKEN_VERIFICATION_FAILED", {ErrorCategory::kAuth, false}},
        {"CX_ERR_AUTH_EMAIL_SEND_FAILED", {ErrorCategory::kTransient, true}},
        {"CX_ERR_AUTH_BCRYPT_TIMEOUT", {ErrorCategory::kPermanent, false}},
        {"CX_ERR_AUTH_JWT_INIT_FAILED", {ErrorCategory::kPermanent, false}},
        // Anti-enumeration (F04 issue 2.6): namespace not-found and unauthorized
        // share this single identity so existence is never leaked.
        {"CX_ERR_NS_UNAUTHORIZED", {ErrorCategory::kAuth, false}},
        // sec.3.3 Store
        {"CX_ERR_STORE_NOT_FOUND", {ErrorCategory::kPermanent, false}},
        {"CX_ERR_STORE_DB_ERROR", {ErrorCategory::kTransient, true}},
        // sec.3.4 F14 pgcortrix
        {"CX_ERR_F14_INVALID_FILTER", {ErrorCategory::kPermanent, false}},
        // sec.3.5 retrieval chain (F36 / F38 / F48)
        {"CX_ERR_RAG_FUSION_LLM_TIMEOUT", {ErrorCategory::kTimeout, true}},
        {"CX_ERR_RAG_FUSION_LLM_QUOTA", {ErrorCategory::kQuota, true}},
        {"CX_ERR_F38_LLM_TIMEOUT", {ErrorCategory::kTimeout, true}},
        {"CX_ERR_F38_PARENT_NOT_FOUND", {ErrorCategory::kPermanent, false}},
        {"CX_ERR_F48_LLM_TIMEOUT", {ErrorCategory::kTimeout, true}},
        {"CX_ERR_F48_LLM_QUOTA_EXCEEDED", {ErrorCategory::kQuota, true}},
        {"CX_ERR_F48_LLM_UNAVAILABLE", {ErrorCategory::kTransient, true}},
        {"CX_ERR_F48_RAG_FAILED", {ErrorCategory::kTransient, true}},
        // sec.3.6 rate limit
        {"CX_ERR_RATE_LIMITED", {ErrorCategory::kQuota, true}},
        // sec.3.7 MEM02 extraction
        {"CX_ERR_MEM02_EXTRACT_LLM_TIMEOUT", {ErrorCategory::kTimeout, true}},
        {"CX_ERR_MEM02_EXTRACT_BUDGET_EXCEEDED", {ErrorCategory::kQuota, false}},
        // sec.3.1 Namespace (NotFoundError -> permanent/false)
        {"CX_ERR_NAMESPACE_NOT_FOUND", {ErrorCategory::kPermanent, false}},
    };
    return kMap;
}

// StatusCode -> GEN-Agent category fallback (ERROR_CODE_SDK_MAP sec.4.1 HTTP-default
// column / AGENT_FRIENDLY #4) for codes the sec.3 map does not list (incl. bare
// Status with no token). The Status primitive is a lossy carrier (catalog_error.h:
// "deliberately do NOT widen cortrix::Status"), so quota/timeout domain codes that
// coarsen onto kPermissionDenied/kInvalidArgument approximate here - the sec.3 map
// is what restores their true category for the high-frequency identities.
ErrorCategory StatusCodeToCategory(StatusCode code) {
    switch (code) {
        case StatusCode::kUnauthenticated:
            return ErrorCategory::kAuth;  // 401
        case StatusCode::kPermissionDenied:
            return ErrorCategory::kAuth;  // 403
        case StatusCode::kInternal:
            return ErrorCategory::kTransient;  // 500
        case StatusCode::kUnavailable:
            return ErrorCategory::kTransient;  // 503
        case StatusCode::kInvalidArgument:     // 400
        case StatusCode::kNotFound:            // 404
        case StatusCode::kAlreadyExists:       // 409
        case StatusCode::kOk:
        default:
            return ErrorCategory::kPermanent;
    }
}

// StatusCode -> retry signal fallback (AGENT_FRIENDLY #6). Only the transient class
// (500 Internal / 503 Unavailable) is retryable; client-fault classes are not.
bool StatusCodeRetryable(StatusCode code) {
    return code == StatusCode::kInternal || code == StatusCode::kUnavailable;
}

std::string OpenApiRoot() {
    const char* env = std::getenv("CORTRIX_OPENAPI_ROOT");
    if (env && *env) return std::string(env);
    return ".";
}

bool ReadFirstFile(const std::vector<std::string>& paths, std::string* content,
                   std::string* path_used) {
    for (const auto& path : paths) {
        std::ifstream in(path, std::ios::binary);
        if (!in) continue;
        std::stringstream ss;
        ss << in.rdbuf();
        *content = ss.str();
        *path_used = path;
        return true;
    }
    return false;
}

std::vector<std::string> OpenApiYamlCandidates() {
    std::string root = OpenApiRoot();
    return {
        root + "/build/openapi.bundled.yaml",
        root + "/api/openapi.yaml",
    };
}

std::vector<std::string> OpenApiJsonCandidates() {
    std::string root = OpenApiRoot();
    return {
        root + "/build/openapi.json",
        root + "/api/openapi.json",
    };
}

bool IsSafeOpenApiAssetPath(const std::string& value) {
    return !value.empty() && value.find("..") == std::string::npos &&
           value.find('\\') == std::string::npos && value[0] != '/';
}

bool ReadOpenApiAsset(const std::string& group, const std::string& rel,
                      std::string* content, std::string* path_used) {
    if ((group != "paths" && group != "components") || !IsSafeOpenApiAssetPath(rel)) {
        return false;
    }
    const std::string root = OpenApiRoot();
    return ReadFirstFile({root + "/api/" + group + "/" + rel}, content, path_used);
}

const char* WebUiContentSecurityPolicy() {
    return "default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline' "
           "https://fonts.googleapis.com; img-src 'self' data:; connect-src 'self'; "
           "font-src 'self' data: https://fonts.gstatic.com; base-uri 'self'; "
           "form-action 'self'; object-src 'none'; frame-ancestors 'none'";
}

httplib::Headers WebUiSecurityHeaders() {
    return {
        {"Content-Security-Policy", WebUiContentSecurityPolicy()},
        {"X-Frame-Options", "DENY"},
        {"X-Content-Type-Options", "nosniff"},
        {"Referrer-Policy", "no-referrer"},
    };
}

void ApplyWebUiSecurityHeaders(httplib::Response& res) {
    for (const auto& header : WebUiSecurityHeaders()) {
        res.set_header(header.first, header.second);
    }
}

void ApplyBaselineSecurityHeaders(httplib::Response& res) {
    res.set_header("X-Frame-Options", "DENY");
    res.set_header("X-Content-Type-Options", "nosniff");
    res.set_header("Referrer-Policy", "no-referrer");
}

bool HasFileLikeSuffix(const std::string& path) {
    const auto slash = path.find_last_of('/');
    const std::string segment = slash == std::string::npos ? path : path.substr(slash + 1);
    return segment.find('.') != std::string::npos;
}

bool ShouldServeSpaFallback(const httplib::Request& req) {
    if (req.method != "GET") return false;
    if (req.path.rfind("/api/", 0) == 0) return false;
    if (req.path.rfind("/assets/", 0) == 0) return false;
    if (HasFileLikeSuffix(req.path)) return false;
    return true;
}

bool LooksLikeInt(const std::string& value) {
    if (value.empty()) return false;
    size_t i = value[0] == '-' ? 1 : 0;
    if (i == value.size()) return false;
    for (; i < value.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(value[i]))) return false;
    }
    return true;
}

bool LooksLikeFloat(const std::string& value) {
    bool dot = false;
    bool digit = false;
    size_t i = value[0] == '-' ? 1 : 0;
    for (; i < value.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(value[i]);
        if (std::isdigit(c)) {
            digit = true;
            continue;
        }
        if (value[i] == '.' && !dot) {
            dot = true;
            continue;
        }
        return false;
    }
    return dot && digit;
}

nlohmann::json YamlScalarToJson(const std::string& value) {
    if (value == "true") return true;
    if (value == "false") return false;
    if (value == "null" || value == "~") return nullptr;
    try {
        if (LooksLikeInt(value)) return std::stoll(value);
        if (LooksLikeFloat(value)) return std::stod(value);
    } catch (...) {
        // Keep the original string if numeric coercion overflows.
    }
    return value;
}

nlohmann::json YamlToJson(const YAML::Node& node) {
    if (!node || node.IsNull()) return nullptr;
    if (node.IsScalar()) return YamlScalarToJson(node.as<std::string>());
    if (node.IsSequence()) {
        nlohmann::json out = nlohmann::json::array();
        for (const auto& child : node) out.push_back(YamlToJson(child));
        return out;
    }
    if (node.IsMap()) {
        nlohmann::json out = nlohmann::json::object();
        for (const auto& item : node) {
            out[item.first.as<std::string>()] = YamlToJson(item.second);
        }
        return out;
    }
    return nullptr;
}

// Lift the rich CX_ERR_* token an Agent should route on. The catalog/tenant/auth
// boundary prefixes the message as "CX_ERR_X: detail" (CatalogStatus / TenantStatus
// / MakeAuthError convention); return "" when the message carries no such token.
std::string ExtractCxToken(const std::string& msg) {
    if (msg.rfind("CX_ERR_", 0) != 0) return "";
    auto end = msg.find_first_of(": ");
    return end == std::string::npos ? msg : msg.substr(0, end);
}

struct ResolvedError {
    std::string code;
    ErrorCategory category;
    bool retryable;
};

// Resolve the GEN-Agent {code, category, retryable} for a Status. Prefers the rich
// CX_ERR_* token (then the sec.3 SoT map for its true category/retryable); falls
// back to the StatusCode-derived generic code + HTTP-default category otherwise.
ResolvedError ResolveError(const Status& status) {
    const std::string token = ExtractCxToken(status.message());
    if (!token.empty()) {
        auto it = Sdk3Map().find(token);
        if (it != Sdk3Map().end()) {
            return {token, it->second.first, it->second.second};
        }
        // Rich token but not in the sec.3 set: keep the precise code, approximate
        // category/retryable from the coarse StatusCode.
        return {token, StatusCodeToCategory(status.code()), StatusCodeRetryable(status.code())};
    }
    return {status.error_code_string(), StatusCodeToCategory(status.code()),
            StatusCodeRetryable(status.code())};
}

}  // namespace

void WriteJsonError(httplib::Response& res, const Status& status, const std::string& request_id) {
    nlohmann::json body;
    // GEN-Agent error schema (AGENT_FRIENDLY sec.3.1 - 6 fields). The legacy
    // code/message/request_id/timestamp are preserved; retryable/category/
    // retry_after_ms/structured_data are added so Agents can route deterministically
    // (R2-C3). code/category/retryable resolve via the CX_ERR_* token + sec.3 SoT
    // map (StatusCode fallback); structured_data defaults to an empty object
    // (always present per principle 7's stable schema).
    const ResolvedError resolved = ResolveError(status);
    body["error"]["code"] = resolved.code;
    body["error"]["message"] = status.message();
    body["error"]["retryable"] = resolved.retryable;
    body["error"]["category"] = agent_friendly::ToString(resolved.category);
    body["error"]["retry_after_ms"] = nullptr;  // coarse Status carries no hint
    body["error"]["structured_data"] = nlohmann::json::object();
    if (!request_id.empty()) {
        body["error"]["request_id"] = request_id;
        res.set_header("X-Request-Id", request_id);
    }
    // Per API_GATEWAY_DESIGN.md section 2.5.1: include ISO 8601 timestamp
    {
        auto now = std::chrono::system_clock::now();
        auto ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
        auto time_t_now = std::chrono::system_clock::to_time_t(now);
        struct tm tm_buf;
        gmtime_r(&time_t_now, &tm_buf);
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm_buf);
        std::ostringstream ts;
        ts << buf << "." << std::setfill('0') << std::setw(3) << ms.count() << "Z";
        body["error"]["timestamp"] = ts.str();
    }
    ApplyBaselineSecurityHeaders(res);
    res.set_content(body.dump(), "application/json");
    res.status = status.http_status();
}

void WriteJsonResponse(httplib::Response& res, int http_status, const nlohmann::json& body,
                       const std::string& request_id) {
    if (!request_id.empty()) {
        res.set_header("X-Request-Id", request_id);
    }
    ApplyBaselineSecurityHeaders(res);
    res.set_content(body.dump(), "application/json");
    res.status = http_status;
}

CortrixHttpServer::CortrixHttpServer(const CortrixConfig& config, ApiKeyAuth& auth,
                                     NamespaceManager& ns_mgr)
    : config_(config), auth_(auth), ns_mgr_(ns_mgr) {}

void CortrixHttpServer::RegisterRoutes() {
    RegisterHealthRoutes();
    RegisterOpenApiRoutes();
    RegisterSystemRoutes();
    RegisterNamespaceRoutes();

    // 404 handler for unmatched routes
    // Uses WriteJsonError() to ensure consistent error format with timestamp
    // per API_GATEWAY_DESIGN.md section 2.5.1. When the web UI is enabled,
    // unmatched non-/api GETs serve index.html instead (SPA deep links).
    svr_.set_error_handler([this](const httplib::Request& req, httplib::Response& res) {
        if (res.status == 404) {
            if (!web_ui_index_.empty() && ShouldServeSpaFallback(req)) {
                res.status = 200;
                ApplyWebUiSecurityHeaders(res);
                res.set_content(web_ui_index_, "text/html");
                return;
            }
            // Only synthesize a generic body when NO handler matched (body still empty).
            // httplib invokes the error handler for ANY >=400 status, so a handler that ran
            // and wrote its own 4xx body (e.g. CX_ERR_F13_SESSION_NOT_FOUND) would otherwise
            // be clobbered into a misleading generic "resource not found".
            if (res.body.empty()) {
                std::string req_id = GenerateRequestId();
                WriteJsonError(res, Status::NotFound("The requested resource was not found"),
                               req_id);
            }
        }
    });

    // Global exception handler (R2-M2). Without this, any exception a route
    // handler fails to catch escapes to cpp-httplib's default, which returns a
    // bare text/plain 500 - an Agent cannot route on it (violates AGENT_FRIENDLY
    // principle 1). Re-emit the GEN-Agent error envelope so the schema stays
    // uniform. An uncaught exception is an internal *bug*, not a transient blip,
    // so it is category=permanent / retryable=false (a blind retry would just
    // re-trigger the same fault), diverging intentionally from the kInternal
    // default WriteJsonError would assign.
    svr_.set_exception_handler([](const httplib::Request& req, httplib::Response& res,
                                  std::exception_ptr ep) {
        agent_friendly::AgentFriendlyError err;
        err.code = "CX_ERR_INTERNAL_ERROR";
        err.category = agent_friendly::ErrorCategory::kPermanent;
        err.retryable = false;
        try {
            if (ep) std::rethrow_exception(ep);
            err.message = "Internal server error";
        } catch (const std::exception& e) {
            err.message = std::string("Internal server error: ") + e.what();
        } catch (...) {
            err.message = "Internal server error: unknown exception";
        }
        // Echo the inbound correlation id when the client supplied one;
        // otherwise mint a fresh one (mirrors the 404 handler).
        std::string req_id = req.has_header("X-Request-Id") ? req.get_header_value("X-Request-Id")
                                                            : GenerateRequestId();
        nlohmann::json body;
        body["error"] = agent_friendly::ToJson(err);
        body["error"]["request_id"] = req_id;
        res.set_header("X-Request-Id", req_id);
        res.set_content(body.dump(), "application/json");
        res.status = 500;
    });
}

void CortrixHttpServer::RegisterOpenApiRoutes() {
    svr_.Get("/docs",
             NoAuth([](const httplib::Request&, httplib::Response& res, const RequestContext&) {
                 const std::string html = R"(<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Cortrix API Docs</title>
  <link rel="stylesheet" href="https://unpkg.com/swagger-ui-dist@5/swagger-ui.css">
</head>
<body>
  <div id="swagger-ui"></div>
  <script src="https://unpkg.com/swagger-ui-dist@5/swagger-ui-bundle.js"></script>
  <script>
    window.ui = SwaggerUIBundle({
      url: "/openapi.json",
      dom_id: "#swagger-ui",
      deepLinking: true,
      displayRequestDuration: true,
      tryItOutEnabled: true
    });
  </script>
</body>
</html>)";
                 res.set_content(html, "text/html");
                 res.status = 200;
             }));

    svr_.Get(R"(/(paths|components)/(.+\.ya?ml))",
             NoAuth([](const httplib::Request& req, httplib::Response& res,
                       const RequestContext& rctx) {
                 std::string content;
                 std::string path;
                 if (!ReadOpenApiAsset(req.matches[1], req.matches[2], &content, &path)) {
                     WriteJsonError(res, Status::NotFound("OpenAPI asset not found"),
                                    rctx.request_id);
                     return;
                 }
                 res.set_header("X-Cortrix-OpenAPI-Source", path);
                 res.set_content(content, "application/yaml");
                 res.status = 200;
             }));

    svr_.Get(
        "/openapi.yaml",
        NoAuth([](const httplib::Request&, httplib::Response& res, const RequestContext& rctx) {
            std::string content;
            std::string path;
            if (!ReadFirstFile(OpenApiYamlCandidates(), &content, &path)) {
                WriteJsonError(res, Status::NotFound("OpenAPI YAML not found"), rctx.request_id);
                return;
            }
            res.set_header("X-Cortrix-OpenAPI-Source", path);
            res.set_content(content, "application/yaml");
            res.status = 200;
        }));

    svr_.Get(
        "/openapi.bundled.yaml",
        NoAuth([](const httplib::Request&, httplib::Response& res, const RequestContext& rctx) {
            std::string content;
            std::string path;
            if (!ReadFirstFile(OpenApiYamlCandidates(), &content, &path)) {
                WriteJsonError(res, Status::NotFound("OpenAPI YAML not found"), rctx.request_id);
                return;
            }
            res.set_header("X-Cortrix-OpenAPI-Source", path);
            res.set_content(content, "application/yaml");
            res.status = 200;
        }));

    svr_.Get(
        "/openapi.json",
        NoAuth([](const httplib::Request&, httplib::Response& res, const RequestContext& rctx) {
            std::string content;
            std::string path;
            if (ReadFirstFile(OpenApiJsonCandidates(), &content, &path)) {
                res.set_header("X-Cortrix-OpenAPI-Source", path);
                res.set_content(content, "application/json");
                res.status = 200;
                return;
            }
            if (!ReadFirstFile(OpenApiYamlCandidates(), &content, &path)) {
                WriteJsonError(res, Status::NotFound("OpenAPI spec not found"), rctx.request_id);
                return;
            }
            try {
                nlohmann::json body = YamlToJson(YAML::Load(content));
                res.set_header("X-Cortrix-OpenAPI-Source", path);
                res.set_content(body.dump(), "application/json");
                res.status = 200;
            } catch (const std::exception& e) {
                WriteJsonError(
                    res, Status::Internal(std::string("OpenAPI YAML parse failed: ") + e.what()),
                    rctx.request_id);
            }
        }));
}

void CortrixHttpServer::EnableWebUi(const std::string& dir) {
    std::ifstream index(dir + "/index.html", std::ios::binary);
    if (!index) {
        CORTRIX_LOG_WARN("server", "Web UI dir '{}' has no readable index.html — UI disabled", dir);
        return;
    }
    std::stringstream ss;
    ss << index.rdbuf();
    web_ui_index_ = ss.str();
    svr_.set_mount_point("/", dir, WebUiSecurityHeaders());
    CORTRIX_LOG_INFO("server", "Web UI enabled from {}", dir);
}

Status CortrixHttpServer::Start() {
    start_time_ = std::chrono::steady_clock::now();
    running_ = true;

    svr_.new_task_queue = [this] { return new httplib::ThreadPool(config_.server.thread_count); };

    CORTRIX_LOG_INFO("server", "Starting HTTP server on {}:{}", config_.server.host,
                     config_.server.port);

    if (!svr_.listen(config_.server.host, config_.server.port)) {
        running_ = false;
        return Status::Internal("Failed to bind to " + config_.server.host + ":" +
                                std::to_string(config_.server.port));
    }

    return Status::Ok();
}

void CortrixHttpServer::Stop() {
    if (running_) {
        running_ = false;
        svr_.stop();
        CORTRIX_LOG_INFO("server", "HTTP server stopped");
    }
}

void CortrixHttpServer::SetNamespacePool(cortrix::resource::INamespacePool* pool) {
    pool_ = pool;
}

void CortrixHttpServer::SetNamespaceRouter(cortrix::catalog::INSRouter* router) {
    ns_router_ = router;
}

void CortrixHttpServer::SetOperationLogger(cortrix::observability::IOperationLogger* op_logger) {
    op_logger_ = op_logger;
}

void CortrixHttpServer::SetEdition(const std::string& edition) {
    edition_ = edition;
}

void CortrixHttpServer::EmitNsLog(const std::string& action, const std::string& name) {
    if (!op_logger_) return;  // observability strictly additive (C4)
    // §9.1 NamespaceManager → resource_type "namespace"; resource_id = the NS name
    // (the namespace_id in this single-SoT model). user_id / trace_id / session_id
    // come from the thread-local ObservabilityContext (request thread, WithAuth).
    auto entry = observability::MakeEngineEntry(observability::EmitSite::kNamespaceManager, action,
                                                /*namespace_id=*/name,
                                                /*resource_id=*/name, action + ": " + name);
    op_logger_->Log(entry);
}

httplib::Server& CortrixHttpServer::server() {
    return svr_;
}

void CortrixHttpServer::RegisterHealthRoutes() {
    svr_.Get("/api/v1/health", NoAuth([this](const httplib::Request&, httplib::Response& res,
                                             const RequestContext& rctx) {
                 auto now = std::chrono::steady_clock::now();
                 auto uptime =
                     std::chrono::duration_cast<std::chrono::seconds>(now - start_time_).count();

                 nlohmann::json body;
                 body["status"] = "healthy";
                 body["version"] = cortrix::kCortrixVersion;
                 body["uptime_seconds"] = uptime;
                 body["components"]["config"] = "ok";
                 body["components"]["logging"] = "ok";
                 body["components"]["namespace_manager"] = "ok";

                 // DEFECT#8: reflect the OCR parser provisioning outcome so a server
                 // whose parser stack failed to install does not silently report a
                 // bare "healthy". The entrypoint sets CORTRIX_PARSER_STATUS after
                 // provisioning (ok / disabled=lite / unavailable=failed). Absent on
                 // dev/native runs → omit the component and keep prior behaviour;
                 // only "unavailable" degrades the overall status.
                 if (const char* parser_status = std::getenv("CORTRIX_PARSER_STATUS")) {
                     if (parser_status[0] != '\0') {
                         body["components"]["parser"] = parser_status;
                         if (std::string(parser_status) == "unavailable") {
                             body["status"] = "degraded";
                         }
                     }
                 }

                 body["llm_enabled"] = config_.semantic_llm.IsConfigured();
                 body["llm_provider"] = config_.semantic_llm.provider;
                 body["llm_model"] = config_.semantic_llm.model;

                 WriteJsonResponse(res, 200, body, rctx.request_id);
             }));
}

void CortrixHttpServer::RegisterSystemRoutes() {
    auto write_ns_stats = [this](const httplib::Request& req, httplib::Response& res,
                                 const RequestContext& rctx) {
        const std::string name = req.path_params.at("ns");
        // Runtime namespace authorization (ARCHITECTURE V6) + anti-enumeration
        // (F04 issue 2.6): unauthorized and not-found share CX_ERR_NS_UNAUTHORIZED
        // and the namespace name is never echoed back.
        if (!auth_.Authorize(rctx.auth, name, kPermRead).ok()) {
            WriteJsonError(res, Status::PermissionDenied("CX_ERR_NS_UNAUTHORIZED"),
                           rctx.request_id);
            return;
        }

        nlohmann::json ns_json;
        Status s = BuildNamespaceJson(name, &ns_json);
        if (!s.ok()) {
            WriteJsonError(res, s, rctx.request_id);
            return;
        }

        nlohmann::json body;
        body["namespace"] = name;
        body["document_count"] = ns_json.value("doc_count", 0);
        body["chunk_count"] = ns_json.value("block_count", 0);
        body["storage_bytes"] = 0;
        body["last_query_at"] = nullptr;
        WriteJsonResponse(res, 200, body, rctx.request_id);
    };

    if (config_.auth.enabled) {
        svr_.Get("/api/v1/system/namespaces/:ns/stats", WithAuth(auth_, kPermRead, write_ns_stats));
    } else {
        svr_.Get("/api/v1/system/namespaces/:ns/stats", NoAuth(write_ns_stats));
    }

    // GET /api/v1/system/features
    // Returns the edition plus which optional features this build provides.
    // Stable schema (Agent-friendly principle 7): unavailable features report
    // false rather than disappearing from the body.
    svr_.Get("/api/v1/system/features", NoAuth([this](const httplib::Request&, httplib::Response& res,
                                                  const RequestContext& rctx) {
                 nlohmann::json body;
                 body["edition"] = edition_;
                 body["features"]["cdc"] = false;
                 body["features"]["text_to_sql"] = false;
                 // Phase 2+ features (not yet implemented)
                 body["features"]["fuse"] = false;
                 body["features"]["s3_proxy"] = false;

                 WriteJsonResponse(res, 200, body, rctx.request_id);
             }));
}

nlohmann::json CortrixHttpServer::BuildNamespaceListJson(const std::vector<std::string>& allowed,
                                                        bool filter) {
    nlohmann::json resp;
    resp["namespaces"] = nlohmann::json::array();
    // Under filter mode an empty authorized set means "see nothing" (deny-by-
    // default); short-circuit so the empty-list paths below don't list all.
    if (filter && allowed.empty()) {
        resp["total"] = 0;
        return resp;
    }
    auto append = [&](const std::string& name, int64_t created_at, int64_t updated_at, int64_t dc,
                      int64_t bc) {
        // Live counts from the store when the NS is resident in the pool
        // (per-request façade); metadata counts otherwise.
        if (pool_) {
            cortrix::resource::NamespaceFacade facade(*pool_, name);
            if (facade.Acquire().ok()) {
                facade.store().doc_count(&dc);
                facade.store().block_count(&bc);
            }
        }
        nlohmann::json j;
        j["name"] = name;
        j["namespace"] = name;
        j["status"] = "active";
        j["created_at"] = created_at;
        j["updated_at"] = updated_at;
        j["doc_count"] = dc;
        j["block_count"] = bc;
        resp["namespaces"].push_back(j);
    };

    if (ns_router_) {
        auto listed = ns_router_->ListNamespaces({});
        if (listed.ok()) {
            for (const auto& ns_id : listed.value().results) {
                if (filter &&
                    std::find(allowed.begin(), allowed.end(), ns_id) == allowed.end()) {
                    continue;
                }
                // NSMetadata has no updated_at column; mirror the create
                // response and reuse created_at.
                int64_t created = 0;
                auto got = ns_router_->GetNamespace(ns_id);
                if (got.ok()) created = got.value().created_at;
                append(ns_id, created, created, 0, 0);
            }
            resp["total"] = resp["namespaces"].size();
            return resp;
        }
    }

    auto nss = ns_mgr_.List(filter ? allowed : std::vector<std::string>{});
    for (const auto& ns : nss) {
        append(ns.name, ns.created_at, ns.updated_at, ns.doc_count, ns.block_count);
    }
    resp["total"] = resp["namespaces"].size();
    return resp;
}

Status CortrixHttpServer::BuildNamespaceJson(const std::string& name, nlohmann::json* out) {
    // Emit an optional string/int as a stable key — JSON null when absent — so the
    // Agent-facing detail schema is fixed-shape (GEN-Agent principle 7: every field
    // always present, never silently dropped on absence).
    auto opt_str = [](const std::optional<std::string>& v) -> nlohmann::json {
        return v.has_value() ? nlohmann::json(*v) : nlohmann::json(nullptr);
    };
    auto opt_int = [](const std::optional<int64_t>& v) -> nlohmann::json {
        return v.has_value() ? nlohmann::json(*v) : nlohmann::json(nullptr);
    };

    int64_t created = 0, updated = 0, dc = 0, bc = 0;
    if (ns_router_) {
        auto got = ns_router_->GetNamespace(name);
        if (!got.ok()) return got.status();
        const cortrix::catalog::NSMetadata& md = got.value();
        created = md.created_at;
        // namespaces (F12 §4.1) has no updated_at column — mirror the create/list
        // responses and surface created_at as updated_at (NSMetadata carries no
        // separate mutation timestamp).
        updated = created;
        // Full catalog identity/relationship fields (F12 §4.1 namespaces row). The
        // prior code emitted only name/created/updated/dc/bc and dropped these 8,
        // so View Details could not show tenant / isolation / visibility / clone
        // lineage / status (F12 Major under-serialization).
        (*out)["namespace_id"] = md.namespace_id;
        (*out)["tenant_id"] = md.tenant_id;
        (*out)["isolation_mode"] = md.isolation_mode;
        (*out)["visibility"] = md.visibility;
        (*out)["owner_user_id"] = opt_str(md.owner_user_id);
        (*out)["status"] = md.status;
        (*out)["cloned_from_ns_id"] = opt_str(md.cloned_from_ns_id);
        (*out)["clone_type"] = opt_str(md.clone_type);
        (*out)["cloned_at"] = opt_int(md.cloned_at);
    } else {
        NamespaceInfo info;
        Status s = ns_mgr_.Get(name, &info);
        if (!s.ok()) return s;
        created = info.created_at;
        updated = info.updated_at;
        dc = info.doc_count;
        bc = info.block_count;
    }
    if (pool_) {
        cortrix::resource::NamespaceFacade facade(*pool_, name);
        if (facade.Acquire().ok()) {
            facade.store().doc_count(&dc);
            facade.store().block_count(&bc);
        }
    }
    (*out)["name"] = name;
    (*out)["namespace"] = name;
    if (!out->contains("status")) {
        (*out)["status"] = "active";
    }
    (*out)["created_at"] = created;
    (*out)["updated_at"] = updated;
    (*out)["doc_count"] = dc;
    (*out)["block_count"] = bc;
    return Status::Ok();
}

Status CortrixHttpServer::DeleteNamespaceUnified(const std::string& name) {
    // Catalog soft-delete also triggers F05 pool eviction (F12 §3.1.bis).
    if (ns_router_) return ns_router_->DeleteNamespace(name);
    return ns_mgr_.Delete(name);
}

void CortrixHttpServer::RegisterNamespaceRoutes() {
    // POST /api/v1/namespaces - Create namespace (ADMIN)
    if (config_.auth.enabled) {
        svr_.Post(
            "/api/v1/namespaces",
            WithAuth(auth_, kPermAdmin,
                     [this](const httplib::Request& req, httplib::Response& res,
                            const RequestContext& rctx) {
                         nlohmann::json body;
                         try {
                             body = nlohmann::json::parse(req.body);
                         } catch (...) {
                             WriteJsonError(res,
                                            Status::InvalidArgument("Invalid JSON in request body"),
                                            rctx.request_id);
                             return;
                         }

                         if (!body.contains("name") || !body["name"].is_string()) {
                             WriteJsonError(res, Status::InvalidArgument("Missing 'name' field"),
                                            rctx.request_id);
                             return;
                         }

                         std::string name = body["name"].get<std::string>();

                         nlohmann::json resp;
                         if (ns_router_) {
                             // F13: route creation through the F12 catalog (single SoT). The
                             // router's Status carries the CX_ERR_NS_* token, so already-exists
                             // → 409 / quota → 403 fall out of WriteJsonError unchanged.
                             cortrix::catalog::NSMetadata meta;
                             meta.namespace_id = name;
                             meta.name = name;
                             // P09 §4.6: tenant ownership from the auth context; the V1.0
                             // OSS single-tenant run (auth off / anonymous) falls back to
                             // the bootstrap 'default_tenant' (namespaces.tenant_id is
                             // NOT NULL + FK, seeded by the F12 schema bootstrap).
                             meta.tenant_id = rctx.auth.tenant_id.empty() ? "default_tenant"
                                                                          : rctx.auth.tenant_id;
                             Status s = ns_router_->CreateNamespace(meta);
                             if (!s.ok()) {
                                 WriteJsonError(res, s, rctx.request_id);
                                 return;
                             }
                             // Read the row back from the same SoT for created_at; a fresh NS
                             // has no documents yet (doc/block counts default to 0).
                             int64_t created_at = 0;
                             auto got = ns_router_->GetNamespace(name);
                             if (got.ok()) created_at = got.value().created_at;
                             resp["name"] = name;
                             resp["namespace"] = name;
                             resp["status"] = "active";
                             resp["created_at"] = created_at;
                             resp["updated_at"] = created_at;
                             resp["doc_count"] = 0;
                             resp["block_count"] = 0;
                         } else {
                             // No catalog router injected (standalone/tests): fall back to the
                             // metadata manager so the endpoint still functions.
                             Status s = ns_mgr_.Create(name);
                             if (!s.ok()) {
                                 WriteJsonError(res, s, rctx.request_id);
                                 return;
                             }
                             NamespaceInfo info;
                             ns_mgr_.Get(name, &info);
                             resp["name"] = info.name;
                             resp["namespace"] = info.name;
                             resp["status"] = "active";
                             resp["created_at"] = info.created_at;
                             resp["updated_at"] = info.updated_at;
                             resp["doc_count"] = info.doc_count;
                             resp["block_count"] = info.block_count;
                         }

                         EmitNsLog("ns_create", name);  // [F18a §9.1] success-only
                         WriteJsonResponse(res, 201, resp, rctx.request_id);
                     }));
    } else {
        svr_.Post(
            "/api/v1/namespaces", NoAuth([this](const httplib::Request& req, httplib::Response& res,
                                                const RequestContext& rctx) {
                nlohmann::json body;
                try {
                    body = nlohmann::json::parse(req.body);
                } catch (...) {
                    WriteJsonError(res, Status::InvalidArgument("Invalid JSON in request body"),
                                   rctx.request_id);
                    return;
                }

                if (!body.contains("name") || !body["name"].is_string()) {
                    WriteJsonError(res, Status::InvalidArgument("Missing 'name' field"),
                                   rctx.request_id);
                    return;
                }

                std::string name = body["name"].get<std::string>();

                nlohmann::json resp;
                if (ns_router_) {
                    // F13: route creation through the F12 catalog (single SoT). The
                    // router's Status carries the CX_ERR_NS_* token, so already-exists
                    // → 409 / quota → 403 fall out of WriteJsonError unchanged.
                    cortrix::catalog::NSMetadata meta;
                    meta.namespace_id = name;
                    meta.name = name;
                    // P09 §4.6: tenant ownership from the auth context; the V1.0
                    // OSS single-tenant run (auth off / anonymous) falls back to
                    // the bootstrap 'default_tenant' (namespaces.tenant_id is
                    // NOT NULL + FK, seeded by the F12 schema bootstrap).
                    meta.tenant_id =
                        rctx.auth.tenant_id.empty() ? "default_tenant" : rctx.auth.tenant_id;
                    Status s = ns_router_->CreateNamespace(meta);
                    if (!s.ok()) {
                        WriteJsonError(res, s, rctx.request_id);
                        return;
                    }
                    // Read the row back from the same SoT for created_at; a fresh NS
                    // has no documents yet (doc/block counts default to 0).
                    int64_t created_at = 0;
                    auto got = ns_router_->GetNamespace(name);
                    if (got.ok()) created_at = got.value().created_at;
                    resp["name"] = name;
                    resp["namespace"] = name;
                    resp["status"] = "active";
                    resp["created_at"] = created_at;
                    resp["updated_at"] = created_at;
                    resp["doc_count"] = 0;
                    resp["block_count"] = 0;
                } else {
                    // No catalog router injected (standalone/tests): fall back to the
                    // metadata manager so the endpoint still functions.
                    Status s = ns_mgr_.Create(name);
                    if (!s.ok()) {
                        WriteJsonError(res, s, rctx.request_id);
                        return;
                    }
                    NamespaceInfo info;
                    ns_mgr_.Get(name, &info);
                    resp["name"] = info.name;
                    resp["namespace"] = info.name;
                    resp["status"] = "active";
                    resp["created_at"] = info.created_at;
                    resp["updated_at"] = info.updated_at;
                    resp["doc_count"] = info.doc_count;
                    resp["block_count"] = info.block_count;
                }

                EmitNsLog("ns_create", name);  // [F18a §9.1] success-only
                WriteJsonResponse(res, 201, resp, rctx.request_id);
            }));
    }

    // GET /api/v1/namespaces - List namespaces (READ)
    if (config_.auth.enabled) {
        svr_.Get("/api/v1/namespaces",
                 WithAuth(auth_, kPermRead,
                          [this](const httplib::Request&, httplib::Response& res,
                                 const RequestContext& rctx) {
                              // Runtime authorized set (ARCHITECTURE V6): show only
                              // namespaces the principal may see.
                              nlohmann::json resp = BuildNamespaceListJson(
                                  auth_.ListAuthorizedNamespaces(rctx.auth), /*filter=*/true);
                              WriteJsonResponse(res, 200, resp, rctx.request_id);
                          }));
    } else {
        svr_.Get("/api/v1/namespaces",
                 NoAuth([this](const httplib::Request&, httplib::Response& res,
                               const RequestContext& rctx) {
                     nlohmann::json resp = BuildNamespaceListJson({}, /*filter=*/false);
                     WriteJsonResponse(res, 200, resp, rctx.request_id);
                 }));
    }

    // GET /api/v1/namespaces/:name - Get namespace (READ)
    if (config_.auth.enabled) {
        svr_.Get("/api/v1/namespaces/:name",
                 WithAuth(auth_, kPermRead,
                          [this](const httplib::Request& req, httplib::Response& res,
                                 const RequestContext& rctx) {
                              std::string name = req.path_params.at("name");

                              nlohmann::json resp;
                              Status s = BuildNamespaceJson(name, &resp);
                              if (!s.ok()) {
                                  WriteJsonError(res, s, rctx.request_id);
                                  return;
                              }

                              WriteJsonResponse(res, 200, resp, rctx.request_id);
                          }));
    } else {
        svr_.Get("/api/v1/namespaces/:name",
                 NoAuth([this](const httplib::Request& req, httplib::Response& res,
                               const RequestContext& rctx) {
                     std::string name = req.path_params.at("name");

                     nlohmann::json resp;
                     Status s = BuildNamespaceJson(name, &resp);
                     if (!s.ok()) {
                         WriteJsonError(res, s, rctx.request_id);
                         return;
                     }

                     WriteJsonResponse(res, 200, resp, rctx.request_id);
                 }));
    }

    // DELETE /api/v1/namespaces/:name - Delete namespace (ADMIN)
    if (config_.auth.enabled) {
        svr_.Delete("/api/v1/namespaces/:name",
                    WithAuth(auth_, kPermAdmin,
                             [this](const httplib::Request& req, httplib::Response& res,
                                    const RequestContext& rctx) {
                                 std::string name = req.path_params.at("name");

                                 Status s = DeleteNamespaceUnified(name);
                                 if (!s.ok()) {
                                     WriteJsonError(res, s, rctx.request_id);
                                     return;
                                 }

                                 EmitNsLog("ns_delete", name);  // [F18a §9.1] success-only
                                 res.set_header("X-Request-Id", rctx.request_id);
                                 res.status = 204;
                             }));
    } else {
        svr_.Delete("/api/v1/namespaces/:name",
                    NoAuth([this](const httplib::Request& req, httplib::Response& res,
                                  const RequestContext& rctx) {
                        std::string name = req.path_params.at("name");

                        Status s = DeleteNamespaceUnified(name);
                        if (!s.ok()) {
                            WriteJsonError(res, s, rctx.request_id);
                            return;
                        }

                        EmitNsLog("ns_delete", name);  // [F18a §9.1] success-only
                        res.set_header("X-Request-Id", rctx.request_id);
                        res.status = 204;
                    }));
    }
}

}  // namespace cortrix
