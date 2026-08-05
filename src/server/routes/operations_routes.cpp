#include "cortrix/server/routes/operations_routes.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cortrix/agent_friendly/error.h"
#include "cortrix/auth/auth_middleware.h"
#include "cortrix/observability/operation_log_error.h"
#include "cortrix/observability/operation_logger.h"
#include "cortrix/server/http_server.h"

namespace cortrix {

namespace {

using observability::IOperationLogger;
using observability::MakeOplogError;
using observability::OperationLogEntry;
using observability::OperationLogFilter;
using observability::OperationLogQueryResult;
using observability::OplogErrorCode;

// --- query-param parsing helpers --------------------------------------------

// Split a comma-separated list, trimming surrounding ASCII whitespace and
// dropping empty tokens ("a,,b" -> {"a","b"}). Used for action_in (§6.1 csv).
std::vector<std::string> SplitCsv(const std::string& s) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ',')) {
        size_t a = item.find_first_not_of(" \t");
        if (a == std::string::npos) continue;
        size_t b = item.find_last_not_of(" \t");
        out.push_back(item.substr(a, b - a + 1));
    }
    return out;
}

// Parse a signed 64-bit param; nullopt if absent or not a clean integer (callers
// turn an unparsable timestamp into CX_ERR_OPLOG_INVALID_FILTER themselves).
std::optional<int64_t> ParseInt64Param(const httplib::Request& req, const char* key) {
    if (!req.has_param(key)) return std::nullopt;
    const std::string v = req.get_param_value(key);
    try {
        size_t pos = 0;
        int64_t n = std::stoll(v, &pos);
        if (pos != v.size()) return std::nullopt;  // trailing junk -> invalid
        return n;
    } catch (...) {
        return std::nullopt;
    }
}

// Per-route RNG for error ids + CLEANUP_RUNNING jitter (thread-local; one engine
// thread serves a request at a time so no extra locking is needed).
std::mt19937_64& Rng() {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    return rng;
}

// 16-hex-char correlation id for CX_ERR_OPLOG_INTERNAL structured_data.error_id
// (the request_id is the primary correlation handle; this is a per-error tag).
std::string MakeErrorId() {
    uint64_t v = Rng()();
    char buf[17];
    snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
    return std::string(buf);
}

// §7.2 CX_ERR_OPLOG_CLEANUP_RUNNING retry_at_ms = base backoff + [0,1000) jitter
// (relative ms the client should wait), so retries from many clients don't thunder.
int CleanupRetryAtMs() {
    std::uniform_int_distribution<int> jitter(0, 999);
    return observability::kOplogCleanupRetryBaseMs + jitter(Rng());
}

// --- CX_ERR_OPLOG_* token recovery (boundary re-inflation) -------------------
// OperationLogger::Query returns a Status whose message is "CX_ERR_OPLOG_X: detail"
// (OplogStatus). We recover the identity here and rebuild the full Agent-friendly
// body — the token-only Status deliberately does not carry structured_data, so the
// API boundary is the SoT for the §7.2 required keys (mirrors batch_submit_service
// ExtractCxCode + import_task_queue re-inflation precedent).

// "CX_ERR_OPLOG_X" from "CX_ERR_OPLOG_X: detail" (or the whole string if no ':').
std::string ExtractCxCode(const std::string& message) {
    if (message.rfind("CX_ERR_OPLOG_", 0) != 0) return "";
    size_t end = message.find_first_of(": \t", 0);
    return end == std::string::npos ? message : message.substr(0, end);
}

// Human-readable detail after the "CX_ERR_OPLOG_X: " prefix ("" if none).
std::string ExtractDetail(const std::string& message) {
    size_t colon = message.find(':');
    if (colon == std::string::npos) return "";
    size_t start = message.find_first_not_of(" \t", colon + 1);
    return start == std::string::npos ? "" : message.substr(start);
}

// Map a CX_ERR_OPLOG_* token back to its enum (kInternal on an unknown token —
// any non-tokenized Query failure is treated as an internal fault).
OplogErrorCode CodeFromToken(const std::string& cx) {
    if (cx == "CX_ERR_OPLOG_INVALID_FILTER")          return OplogErrorCode::kInvalidFilter;
    if (cx == "CX_ERR_OPLOG_INVALID_TIMESTAMP_RANGE") return OplogErrorCode::kInvalidTimestampRange;
    if (cx == "CX_ERR_OPLOG_PAGINATION_OUT_OF_RANGE") return OplogErrorCode::kPaginationOutOfRange;
    if (cx == "CX_ERR_OPLOG_UNAUTHORIZED")            return OplogErrorCode::kUnauthorized;
    if (cx == "CX_ERR_OPLOG_CLEANUP_RUNNING")         return OplogErrorCode::kCleanupRunning;
    return OplogErrorCode::kInternal;
}

// Best-effort recover "total_count N" from the pagination detail string
// ("offset X >= total_count Y") so the §7.2 {offset,total_count} body is exact.
std::optional<int64_t> ParseTrailingTotalCount(const std::string& detail) {
    const std::string key = "total_count ";
    size_t p = detail.rfind(key);
    if (p == std::string::npos) return std::nullopt;
    try {
        return std::stoll(detail.substr(p + key.size()));
    } catch (...) {
        return std::nullopt;
    }
}

// Serialize one operation_log row to the §6.1 response shape (optional columns
// become JSON null when NULL in the DB).
nlohmann::json EntryToJson(const OperationLogEntry& e) {
    auto opt = [](const std::optional<std::string>& v) {
        return v.has_value() ? nlohmann::json(*v) : nlohmann::json(nullptr);
    };
    nlohmann::json j;
    j["id"]            = e.id;
    j["timestamp"]     = e.timestamp;
    j["user_id"]       = e.user_id;
    j["action"]        = e.action;
    j["namespace_id"]  = opt(e.namespace_id);
    j["resource_type"] = e.resource_type;
    j["resource_id"]   = opt(e.resource_id);
    j["summary"]       = opt(e.summary);
    j["trace_id"]      = opt(e.trace_id);
    j["session_id"]    = opt(e.session_id);
    return j;
}

// Write a CX_ERR_OPLOG_* error as the GEN-Agent 4-field body (§7.1/§7.2).
// `structured_data` carries the §7.2 required keys for `code`; category /
// retryable / retry_after_ms come from the canonical registry via MakeOplogError.
// The HTTP status is the StatusCode→http mapping of the code.
void WriteOplogError(httplib::Response& res, OplogErrorCode code,
                     nlohmann::json structured_data, const std::string& message,
                     const std::string& request_id) {
    auto err = MakeOplogError(code, std::move(structured_data), message);

    nlohmann::json body;
    body["error"] = agent_friendly::ToJson(err);
    if (!request_id.empty()) {
        body["error"]["request_id"] = request_id;
        res.set_header("X-Request-Id", request_id);
    }
    res.set_content(body.dump(), "application/json");
    res.status = Status(observability::OplogErrorToStatusCode(code), "").http_status();
}

}  // namespace

void RegisterOperationsRoutes(httplib::Server& server,
                              IOperationLogger& logger,
                              ApiKeyAuth& auth) {
    server.Get("/api/v1/operations",
        WithAuth(auth, kPermRead,
            [&logger](const httplib::Request& req, httplib::Response& res,
                      const RequestContext& rctx) {
                OperationLogFilter filter;

                // ---- string filters (§6.1) ----
                if (req.has_param("action"))
                    filter.action = req.get_param_value("action");
                if (req.has_param("namespace_id"))
                    filter.namespace_id = req.get_param_value("namespace_id");
                if (req.has_param("resource_type"))
                    filter.resource_type = req.get_param_value("resource_type");
                if (req.has_param("trace_id"))
                    filter.trace_id = req.get_param_value("trace_id");
                if (req.has_param("action_in"))
                    filter.action_in = SplitCsv(req.get_param_value("action_in"));

                // ---- user_id scope + admin cross-user check (§6.1) ----
                // Default scope = the caller's own user_id. A request for someone
                // else's rows requires an admin key, else CX_ERR_OPLOG_UNAUTHORIZED.
                const std::string& self = rctx.auth.user_id;
                if (req.has_param("user_id")) {
                    const std::string requested = req.get_param_value("user_id");
                    if (requested != self && !rctx.auth.is_admin()) {
                        WriteOplogError(res, OplogErrorCode::kUnauthorized,
                                        {{"required_role", "admin"}},
                                        "cross-user operation-log query requires admin role",
                                        rctx.request_id);
                        return;
                    }
                    filter.user_id = requested;
                } else if (!self.empty()) {
                    // No user_id given: scope to the caller's own rows (§6.1 default).
                    filter.user_id = self;
                }
                // else: self is empty AND admin — the only case is auth-disabled dev
                // mode (WithAuth grants full perms with an empty AuthContext). Leave
                // user_id unconstrained so the operator sees all rows; a real
                // non-admin always has a non-empty user_id and is scoped above.

                // ---- timestamp range (§6.1; unparsable -> INVALID_FILTER) ----
                if (req.has_param("from_timestamp")) {
                    auto v = ParseInt64Param(req, "from_timestamp");
                    if (!v.has_value()) {
                        WriteOplogError(res, OplogErrorCode::kInvalidFilter,
                                        {{"invalid_field", "from_timestamp"},
                                         {"reason", "not an integer (Unix ms)"}},
                                        "from_timestamp must be an integer (Unix ms)",
                                        rctx.request_id);
                        return;
                    }
                    filter.from_timestamp = v;
                }
                if (req.has_param("to_timestamp")) {
                    auto v = ParseInt64Param(req, "to_timestamp");
                    if (!v.has_value()) {
                        WriteOplogError(res, OplogErrorCode::kInvalidFilter,
                                        {{"invalid_field", "to_timestamp"},
                                         {"reason", "not an integer (Unix ms)"}},
                                        "to_timestamp must be an integer (Unix ms)",
                                        rctx.request_id);
                        return;
                    }
                    filter.to_timestamp = v;
                }

                // ---- pagination + sort (§6.1) ----
                if (req.has_param("limit")) {
                    auto v = ParseInt64Param(req, "limit");
                    if (!v.has_value()) {
                        WriteOplogError(res, OplogErrorCode::kInvalidFilter,
                                        {{"invalid_field", "limit"},
                                         {"reason", "not an integer"}},
                                        "limit must be an integer", rctx.request_id);
                        return;
                    }
                    filter.limit = static_cast<int>(*v);
                }
                if (req.has_param("offset")) {
                    auto v = ParseInt64Param(req, "offset");
                    if (!v.has_value()) {
                        WriteOplogError(res, OplogErrorCode::kInvalidFilter,
                                        {{"invalid_field", "offset"},
                                         {"reason", "not an integer"}},
                                        "offset must be an integer", rctx.request_id);
                        return;
                    }
                    filter.offset = static_cast<int>(*v);
                }
                if (req.has_param("sort_order")) {
                    // Uppercase so "asc"/"desc" are accepted (§6.1 ASC|DESC). An
                    // out-of-set value falls through to Query's INVALID_FILTER.
                    std::string so = req.get_param_value("sort_order");
                    std::transform(so.begin(), so.end(), so.begin(),
                                   [](unsigned char c) { return std::toupper(c); });
                    filter.sort_order = so;
                }

                // ---- delegate to the logger (authoritative validation + read) ----
                Result<OperationLogQueryResult> r = logger.Query(filter);
                if (!r.ok()) {
                    // Re-inflate the CX_ERR_OPLOG_* token into the full body, rebuilding
                    // the §7.2 structured_data from what the request already knows.
                    const std::string& msg = r.status().message();
                    const std::string cx = ExtractCxCode(msg);
                    const std::string detail = ExtractDetail(msg);
                    const OplogErrorCode code = CodeFromToken(cx);

                    nlohmann::json sd;
                    switch (code) {
                        case OplogErrorCode::kInvalidTimestampRange:
                            sd = {{"from", filter.from_timestamp.value_or(0)},
                                  {"to",   filter.to_timestamp.value_or(0)}};
                            break;
                        case OplogErrorCode::kPaginationOutOfRange: {
                            int64_t total = ParseTrailingTotalCount(detail).value_or(0);
                            sd = {{"offset", filter.offset}, {"total_count", total}};
                            break;
                        }
                        case OplogErrorCode::kUnauthorized:
                            sd = {{"required_role", "admin"}};
                            break;
                        case OplogErrorCode::kCleanupRunning:
                            // §7.2: retry_at_ms = base backoff + jitter (relative ms).
                            sd = {{"retry_at_ms", CleanupRetryAtMs()}};
                            break;
                        case OplogErrorCode::kInternal:
                            sd = {{"error_id", MakeErrorId()}};
                            break;
                        case OplogErrorCode::kInvalidFilter:
                        default:
                            sd = {{"invalid_field", "filter"},
                                  {"reason", detail.empty() ? std::string("invalid filter")
                                                            : detail}};
                            break;
                    }
                    WriteOplogError(res, code, std::move(sd),
                                    detail.empty() ? cx : detail, rctx.request_id);
                    return;
                }

                // ---- success: §6.1 response shape (Lead-decided top-level keys) ----
                const OperationLogQueryResult& qr = *r;
                nlohmann::json ops = nlohmann::json::array();
                for (const auto& e : qr.entries) ops.push_back(EntryToJson(e));

                nlohmann::json body;
                body["operations"]  = std::move(ops);
                body["total_count"] = qr.total_count;
                body["limit"]       = filter.limit;
                body["offset"]      = filter.offset;
                // Pagination hints (§6.1 meta) kept under meta for Agent paging.
                body["meta"]["total_count"] = qr.total_count;
                body["meta"]["has_next"]    = qr.has_next;
                body["meta"]["next_offset"] = qr.next_offset;

                WriteJsonResponse(res, 200, body, rctx.request_id);
            }
        )
    );
}

}  // namespace cortrix
