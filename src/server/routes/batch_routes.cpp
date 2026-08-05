#include "cortrix/server/routes/batch_routes.h"

#include <algorithm>
#include <string>

#include <nlohmann/json.hpp>

#include "cortrix/agent_friendly/error.h"
#include "cortrix/auth/auth_middleware.h"
#include "cortrix/common/json_depth.h"  // metadata depth guard (DoS: deep-JSON dump)
#include "cortrix/server/batch_submit_service.h"
#include "cortrix/server/http_server.h"

namespace cortrix {

namespace {

/// Upper bound on a caller-supplied doc_id. Generous enough for any real external
/// corpus identifier (ULIDs are 26 chars, the longest BEIR-style ids far less)
/// while keeping an unbounded string out of the task row, logs and error bodies.
constexpr std::size_t kMaxDocIdBytes = 512;

// Parse the JSON body into a server::BatchRequest. On a malformed request (bad
// JSON / wrong types / V1-unsupported options) writes the standard error envelope
// and returns false; the batch-level domain faults (size/payload/empty/dup) are
// the service's job (GEN-Agent CX_ERR_BATCH_* envelope), NOT here.
bool ParseBatchRequest(const httplib::Request& req, httplib::Response& res,
                       const std::string& request_id, server::BatchRequest* out) {
    nlohmann::json body;
    try {
        body = nlohmann::json::parse(req.body);
    } catch (...) {
        WriteJsonError(res, Status::InvalidArgument("Invalid JSON in request body"),
                       request_id);
        return false;
    }

    if (!body.is_object()) {
        WriteJsonError(res, Status::InvalidArgument("request body must be a JSON object"),
                       request_id);
        return false;
    }

    if (!body.contains("namespace") || !body["namespace"].is_string()) {
        WriteJsonError(res, Status::InvalidArgument("missing or invalid 'namespace'"),
                       request_id);
        return false;
    }
    out->namespace_id = body["namespace"].get<std::string>();
    // Reject an empty namespace, matching the flat-document surface
    // (flat_document_routes rejects an empty ?namespace= with 400). Documents
    // are namespace-scoped; an empty string would silently create a junk ""
    // partition instead of surfacing the caller's missing-namespace mistake.
    if (out->namespace_id.empty()) {
        WriteJsonError(res, Status::InvalidArgument("'namespace' must be non-empty"),
                       request_id);
        return false;
    }

    if (!body.contains("documents") || !body["documents"].is_array()) {
        WriteJsonError(res, Status::InvalidArgument("missing or invalid 'documents' array"),
                       request_id);
        return false;
    }

    // Each item must be an object with a string doc_id + string content. (An empty
    // / oversized / duplicate-doc_id array is a batch-level domain fault handled by
    // the service — here we only reject structurally malformed items.)
    for (const auto& item : body["documents"]) {
        if (!item.is_object() || !item.contains("doc_id") ||
            !item["doc_id"].is_string() || !item.contains("content") ||
            !item["content"].is_string()) {
            WriteJsonError(res,
                Status::InvalidArgument(
                    "each document requires string 'doc_id' and 'content'"),
                request_id);
            return false;
        }
        server::BatchDocument doc;
        doc.doc_id = item["doc_id"].get<std::string>();
        // doc_id is caller-controlled and lands in the tasks/documents rows, the
        // operation log and every error envelope. It no longer forms any filesystem
        // path (SEC-BATCH-001 — BatchSubmitService mints the on-disk name itself), so
        // separators stay legal: external corpus ids routinely contain them and
        // rejecting them would break existing callers for no security gain. What is
        // rejected is what has no legitimate use and corrupts downstream consumers:
        // NUL / control bytes (log and C-string truncation) and unbounded length.
        if (doc.doc_id.size() > kMaxDocIdBytes) {
            WriteJsonError(res,
                Status::InvalidArgument("'doc_id' exceeds " +
                                        std::to_string(kMaxDocIdBytes) + " bytes"),
                request_id);
            return false;
        }
        if (doc.doc_id.find_first_of(std::string("\0", 1) + "\r\n\t") != std::string::npos ||
            std::any_of(doc.doc_id.begin(), doc.doc_id.end(),
                        [](unsigned char c) { return c < 0x20 || c == 0x7f; })) {
            WriteJsonError(res,
                Status::InvalidArgument("'doc_id' must not contain control characters"),
                request_id);
            return false;
        }
        doc.content = item["content"].get<std::string>();
        // Optional client filename — its extension drives server-side parser
        // selection (batch submit §2.2 documents[] item; ".txt" fallback applied
        // downstream when absent).
        if (item.contains("filename") && item["filename"].is_string()) {
            doc.filename = item["filename"].get<std::string>();
        }
        if (item.contains("metadata") && item["metadata"].is_object()) {
            // Reject over-deep metadata before dump() (json_depth.h): a deeply nested
            // object would otherwise overflow the stack inside dump().
            const int meta_depth = JsonMaxDepth(item["metadata"]);
            if (meta_depth > kMaxMetadataDepth) {
                nlohmann::json error_body;
                error_body["error"] =
                    agent_friendly::ToJson(MakeMetadataTooDeepError(meta_depth));
                WriteJsonResponse(res, 422, error_body, request_id);
                return false;
            }
            doc.metadata_json = item["metadata"].dump();
        }
        out->documents.push_back(std::move(doc));
    }

    // options (batch submit §2.2): async defaults true (must be true in V1);
    // on_duplicate defaults skip.
    out->async = true;
    out->on_duplicate = server::OnDuplicate::kSkip;
    if (body.contains("options") && body["options"].is_object()) {
        const auto& opts = body["options"];
        if (opts.contains("async") && opts["async"].is_boolean()) {
            out->async = opts["async"].get<bool>();
        }
        if (opts.contains("on_duplicate") && opts["on_duplicate"].is_string()) {
            const std::string od = opts["on_duplicate"].get<std::string>();
            if (od == "skip") {
                out->on_duplicate = server::OnDuplicate::kSkip;
            } else if (od == "overwrite") {
                out->on_duplicate = server::OnDuplicate::kOverwrite;
            } else if (od == "error") {
                out->on_duplicate = server::OnDuplicate::kError;
            } else {
                WriteJsonError(res,
                    Status::InvalidArgument(
                        "options.on_duplicate must be one of: skip, overwrite, error"),
                    request_id);
                return false;
            }
        }
    }
    // §2.2 topic 3 — V1 is always async; an explicit async=false is unsupported.
    if (!out->async) {
        WriteJsonError(res,
            Status::InvalidArgument("synchronous batch (async=false) is not supported in V1"),
            request_id);
        return false;
    }

    return true;
}

}  // namespace

void RegisterBatchRoutes(httplib::Server& server,
                         server::BatchSubmitService& service,
                         ApiKeyAuth& auth) {
    // POST /api/v1/documents/batch — batch submit Agent-first batch submit.
    server.Post("/api/v1/documents/batch",
        WithAuth(auth, kPermWrite,
            [&service](const httplib::Request& req, httplib::Response& res,
                       const RequestContext& rctx) {
                server::BatchRequest batch;
                if (!ParseBatchRequest(req, res, rctx.request_id, &batch)) {
                    return;  // ParseBatchRequest already wrote the error envelope
                }

                server::BatchHttpResult result = service.Submit(batch);
                if (!rctx.request_id.empty()) {
                    res.set_header("X-Request-Id", rctx.request_id);
                }
                res.status = result.status;
                res.set_content(result.body.dump(), "application/json");
            }
        )
    );
}

}  // namespace cortrix
