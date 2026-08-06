#include <cstdint>
#include "cortrix/server/batch_submit_service.h"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <set>
#include <utility>

#include "cortrix/agent_friendly/error.h"
#include "cortrix/async/task_info.h"
#include "cortrix/id/ulid.h"
#include "cortrix/query/content_hash.h"
#include "cortrix/spc/parser_factory.h"

namespace cortrix::server {

using agent_friendly::ErrorCategory;

namespace {

// --- materialization path safety (SEC-BATCH-001) ---------------------------

/// The client filename selects the parser through the materialized filepath's
/// extension (ParseDocument → ExtractExtension → ExtensionToMimeType), so the
/// extension has to survive verbatim — this is deliberately NOT filtered down to
/// the parser-backed set. Doing that would silently turn every submission with an
/// unsupported extension (".json", ".csv") from a CX_ERR_UNSUPPORTED_FORMAT
/// failure into a successful plain-text ingest: a behavior change that does not
/// belong in a security fix and has to be decided on its own merits.
///
/// What IS enforced is the character shape, so nothing structural can ride in on
/// the extension: lowercase alphanumerics only, length-capped. Everything else —
/// including no extension at all — falls back to ".txt", matching the previous
/// behavior for a filename with no extension.
std::string SafeExtension(const std::string& filename) {
    const std::string ext = spc::DocumentParserFactory::ExtractExtension(filename);
    constexpr std::size_t kMaxExtChars = 16;
    if (ext.empty() || ext.size() > kMaxExtChars) return ".txt";
    for (const unsigned char c : ext) {
        if (!std::isalnum(c)) return ".txt";  // ExtractExtension already lowercased
    }
    return "." + ext;
}

/// True when `path` resolves strictly inside `base`. Both sides are resolved
/// with weakly_canonical so ".." segments and symlinks are collapsed before the
/// comparison rather than compared textually.
bool IsWithin(const std::filesystem::path& path, const std::filesystem::path& base) {
    std::error_code ec;
    const std::filesystem::path canon_base = std::filesystem::weakly_canonical(base, ec);
    if (ec) return false;
    const std::filesystem::path canon_path = std::filesystem::weakly_canonical(path, ec);
    if (ec) return false;
    const std::filesystem::path rel = std::filesystem::relative(canon_path, canon_base, ec);
    if (ec || rel.empty()) return false;
    return *rel.begin() != "..";
}

// --- per-doc failure classification (batch submit §2.4.2) -------------------
//
// per-doc failures REUSE the originating Feature's existing CX_ERR_* code (no new
// BATCH-per-doc codes — §2.4.2 "avoid error-code explosion"). The submit seam
// (ITaskSubmitter, prod = TaskScheduler::Enqueue) hands back a coarse Status
// whose message carries the "CX_ERR_X: detail" token (the F42Status / catalog
// /pool error-bridge convention). To re-inflate the GEN-Agent 5-field meta.failed[]
// item we map that token → {category, retryable, retry_after_ms}.
//
// This table is the §2.4.2 reuse set verbatim from the detailed design (the
// CX_ERR_NS_QUOTA_EXCEEDED row cross-checked against catalog_error.cpp /
// pool_error.cpp = quota/false). Codes not listed fall back to permanent /
// non-retryable (the safe GEN-Agent default — an Agent will not blindly retry an
// unclassified failure). D3.5 may centralize this into a single repo-wide
// code registry; standalone, the batch layer owns the contract it presents.
struct PerDocErrorInfo {
    ErrorCategory category;
    bool retryable;
    std::optional<int> retry_after_ms;
};

const PerDocErrorInfo& ClassifyPerDocCode(const std::string& cx_code) {
    static const PerDocErrorInfo kQuota      {ErrorCategory::kQuota,     false, std::nullopt};
    static const PerDocErrorInfo kDupPerm    {ErrorCategory::kPermanent, false, std::nullopt};
    static const PerDocErrorInfo kLlmRate    {ErrorCategory::kTransient, true,  5000};
    static const PerDocErrorInfo kDiskFull   {ErrorCategory::kPermanent, false, std::nullopt};
    static const PerDocErrorInfo kInvalid    {ErrorCategory::kPermanent, false, std::nullopt};
    // task enqueue-path transient (tasks_db down) — surfaced when the seam itself
    // fails rather than an upstream per-doc check (retry the doc).
    static const PerDocErrorInfo kServiceDown{ErrorCategory::kTransient, true,  10000};
    static const PerDocErrorInfo kDefault    {ErrorCategory::kPermanent, false, std::nullopt};

    if (cx_code == "CX_ERR_NS_QUOTA_EXCEEDED")          return kQuota;       // catalog (quota)
    if (cx_code == "CX_ERR_DOC_ALREADY_EXISTS")         return kDupPerm;     // scheduler (permanent)
    if (cx_code == "CX_ERR_TRANSIENT_LLM_RATE_LIMIT")   return kLlmRate;     // enricher (transient, retry_after_ms=5000)
    if (cx_code == "CX_ERR_DISK_FULL")                  return kDiskFull;    // deployment (permanent)
    if (cx_code == "CX_ERR_INVALID_CONTENT")            return kInvalid;     // parser (permanent)
    if (cx_code == "CX_ERR_SERVICE_UNAVAILABLE")        return kServiceDown; // scheduler (transient)
    return kDefault;
}

// Recover the "CX_ERR_*" token from a Status whose message is "CX_ERR_X: detail"
// (or just "CX_ERR_X"). Returns "" if the message does not start with CX_ERR_.
std::string ExtractCxCode(const std::string& message) {
    if (message.rfind("CX_ERR_", 0) != 0) return "";
    size_t end = message.find_first_of(": \t", 0);
    return end == std::string::npos ? message : message.substr(0, end);
}

// Human-readable detail after the "CX_ERR_X: " prefix ("" if none).
std::string ExtractDetail(const std::string& message) {
    size_t colon = message.find(':');
    if (colon == std::string::npos) return "";
    size_t start = message.find_first_not_of(" \t", colon + 1);
    return start == std::string::npos ? "" : message.substr(start);
}

}  // namespace

BatchSubmitService::BatchSubmitService(ITaskSubmitter* submitter, BatchLimits limits)
    : submitter_(submitter), limits_(std::move(limits)) {}

std::optional<BatchSubmitService::EnvelopeError>
BatchSubmitService::ValidateEnvelope(const BatchRequest& req) const {
    // §2.4.1 order: empty → size → payload → duplicate doc_id. (Empty first so a
    // 0-doc request is BATCH_EMPTY, not a vacuous success.)
    const int n = static_cast<int>(req.documents.size());

    if (n == 0) {
        return EnvelopeError{BatchErrorCode::kEmpty, nlohmann::json::object(),
                             "documents must contain 1-100 items"};
    }

    if (n > limits_.max_documents) {
        return EnvelopeError{
            BatchErrorCode::kSizeExceeded,
            {{"max_size", limits_.max_documents}, {"actual_size", n}},
            "batch exceeds the maximum document count"};
    }

    // total payload = sum of content bytes (the request body size proxy; §2.2
    // 100MB total). Per-doc 10MB cap is also a payload fault (same code/category).
    int64_t total_bytes = 0;
    for (const BatchDocument& d : req.documents) {
        const int64_t doc_bytes = static_cast<int64_t>(d.content.size());
        total_bytes += doc_bytes;
        if (doc_bytes > limits_.max_doc_bytes || total_bytes > limits_.max_payload_bytes) {
            return EnvelopeError{
                BatchErrorCode::kPayloadTooLarge,
                {{"max_bytes", limits_.max_payload_bytes}, {"actual_bytes", total_bytes}},
                "batch payload exceeds the maximum size"};
        }
    }

    // duplicate doc_id within one batch (§2.4.1 topic 4 Q2). Collect ALL offenders
    // so the Agent can fix every collision in one pass.
    std::set<std::string> seen;
    std::set<std::string> dups;
    for (const BatchDocument& d : req.documents) {
        if (!seen.insert(d.doc_id).second) dups.insert(d.doc_id);
    }
    if (!dups.empty()) {
        nlohmann::json dup_array = nlohmann::json::array();
        for (const std::string& id : dups) dup_array.push_back(id);
        return EnvelopeError{
            BatchErrorCode::kDuplicateDocId,
            {{"duplicate_doc_ids", std::move(dup_array)}},
            "duplicate doc_id within the batch"};
    }

    return std::nullopt;
}

nlohmann::json BatchSubmitService::MakeResultItem(const std::string& doc_id,
                                                  const std::string& task_id,
                                                  const std::string& status) {
    nlohmann::json item;
    item["doc_id"] = doc_id;
    item["task_id"] = task_id;
    item["status"] = status;
    return item;
}

nlohmann::json BatchSubmitService::MakeFailureItem(const std::string& doc_id,
                                                   const Status& status) {
    const std::string cx_code = ExtractCxCode(status.message());
    const std::string detail = ExtractDetail(status.message());
    const PerDocErrorInfo& info = ClassifyPerDocCode(cx_code);

    // batch submit meta.failed[] item: doc_id + the GEN-Agent 5 fields. NOTE
    // the field is `error_code` (NOT the bare top-level-envelope `code`) — this is
    // a partial-success *failure descriptor*, matching the example and the
    // existing cross-NS QueryMeta.namespaces_failed[] convention (error_code /
    // category / retryable / retry_after_ms). error_code falls back to a generic
    // permanent token if the seam returned a non-CX_ERR_ message.
    nlohmann::json item;
    item["doc_id"] = doc_id;
    item["error_code"] = cx_code.empty() ? "CX_ERR_INTERNAL_ERROR" : cx_code;
    item["category"] = agent_friendly::ToString(info.category);
    item["retryable"] = info.retryable;
    item["retry_after_ms"] = info.retry_after_ms.has_value()
        ? nlohmann::json(*info.retry_after_ms) : nlohmann::json(nullptr);
    // structured_data left null at the batch layer — per-doc structured_data is
    // produced by the originating Feature when the real pipeline is wired (D3.5);
    // standalone the 5th field is present-and-null (schema-complete).
    item["structured_data"] = nlohmann::json(nullptr);
    // message: human-readable detail (the token prefix stripped).
    item["message"] = detail.empty() ? status.message() : detail;
    return item;
}

std::string BatchSubmitService::MaterializeContent(const std::string& content,
                                                   const std::string& filename) const {
    if (materialize_dir_.empty()) return "";  // disabled (mock/standalone seam)
    std::error_code ec;
    std::filesystem::create_directories(materialize_dir_, ec);
    if (ec) return "";  // dir unavailable → "" filepath → per-doc parse failure

    // [SEC-BATCH-001] The on-disk name is minted server-side and is NEVER derived
    // from caller input. It used to be the client's doc_id, which made this a
    // caller-directed file write: a relative doc_id ("../../x") walked out of the
    // materialize dir, and an absolute one ("/etc/x") made operator/ discard the
    // directory entirely, so the content landed wherever the caller pointed —
    // overwriting any existing file the process could write. doc_id carries its
    // identity role through SubmitRequest.doc_id and the tasks/documents rows;
    // nothing needs it to appear in a filename. The parameter is gone rather than
    // merely unused so the property cannot regress without changing the signature.
    const std::string stem = id::GenerateUlid();
    const std::filesystem::path path =
        std::filesystem::path(materialize_dir_) / (stem + SafeExtension(filename));

    // Defense in depth: even if the name construction above ever regresses, a
    // path that resolves outside the materialize dir is refused, not written.
    if (!IsWithin(path, materialize_dir_)) return "";

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return "";
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    out.close();
    return out ? path.string() : std::string("");
}

BatchHttpResult BatchSubmitService::Submit(const BatchRequest& req) {
    // 1) batch-level envelope validation → single GEN-Agent CX_ERR_BATCH_* reply.
    if (auto env_err = ValidateEnvelope(req)) {
        BatchHttpResult r;
        r.status = BatchErrorHttpStatus(env_err->code);
        r.body["error"] = agent_friendly::ToJson(
            MakeBatchError(env_err->code, std::move(env_err->structured_data),
                           env_err->message));
        return r;
    }

    // 2) per-doc fan-out through the submit seam (prod = TaskScheduler).
    //
    // on_duplicate (skip/overwrite/error) enforcement is not wired yet: the frozen task
    // SubmitRequest carries no on_duplicate field, and the duplicate decision
    // needs the real store/task dedup (cross-component wiring). Enqueue already
    // applies same-doc_id+same-content_hash debounce/merge; the on_duplicate
    // overwrite→cancel-running path is the overwrite/cancel item (integration). Standalone, every
    // accepted doc is reported "submitted"; the "skipped" status variant becomes
    // reachable when that wiring lands. req.on_duplicate is parsed + validated
    // upstream so the contract is honored end-to-end at D3.5.
    nlohmann::json results = nlohmann::json::array();
    nlohmann::json succeeded = nlohmann::json::array();
    nlohmann::json failed = nlohmann::json::array();
    int succeeded_count = 0;

    for (const BatchDocument& d : req.documents) {
        async::SubmitRequest sreq;
        sreq.namespace_id = req.namespace_id;
        sreq.doc_id = d.doc_id;
        sreq.content_hash = query::ContentHashOfContent(d.content);
        // Materialize the inline content to a server-side file the task
        // doc-parse worker reads (DocumentProcessor → factory.ParseDocument(filepath)).
        // Disabled (materialize_dir_ == "") → "" filepath, the standalone/mock seam.
        // The client filename's extension drives parser selection (".txt" fallback).
        sreq.filepath = MaterializeContent(d.content, d.filename);
        sreq.filename = d.filename.empty() ? (d.doc_id + ".txt") : d.filename;
        sreq.metadata_json = d.metadata_json;  // carry caller doc metadata through the async task
        sreq.task_type = async::kTaskDocParse;

        Result<async::TaskInfo> r = submitter_->Submit(sreq);
        if (!r.ok() && !sreq.filepath.empty() && input_releaser_) {
            // The submission produced no task, so the file just written has no owner
            // and nothing will ever read it. Hand it back now rather than leave it
            // for a sweep — a rejected batch can otherwise deposit one file per doc.
            input_releaser_(sreq.filepath);
        }
        if (r.ok()) {
            results.push_back(MakeResultItem(d.doc_id, r.value().task_id, "submitted"));
            succeeded.push_back(d.doc_id);
            ++succeeded_count;
        } else {
            failed.push_back(MakeFailureItem(d.doc_id, r.status()));
        }
    }

    const int total = static_cast<int>(req.documents.size());
    const double coverage = total > 0
        ? static_cast<double>(succeeded_count) / static_cast<double>(total)
        : 0.0;

    nlohmann::json meta;
    meta["succeeded"] = std::move(succeeded);
    meta["failed"] = std::move(failed);
    meta["coverage_ratio"] = coverage;
    meta["total_submitted"] = total;

    BatchHttpResult out;
    out.status = 200;  // §2.3 — partial-success is still 200; inspect meta.failed[]
    out.body = {{"results", std::move(results)}, {"meta", std::move(meta)}};
    return out;
}

}  // namespace cortrix::server
