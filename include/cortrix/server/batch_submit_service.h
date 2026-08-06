#pragma once
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cortrix/server/batch_error.h"
#include "cortrix/server/i_task_submitter.h"

namespace cortrix::server {

/// One document of a parsed batch request (documents[] item).
struct BatchDocument {
    std::string doc_id;            ///< client-provided, unique within the batch
    std::string content;           ///< inline content (url / file_path are Phase 2)
    std::string metadata_json;     ///< optional raw JSON object string ("" if absent)
    std::string filename;          ///< optional client filename; its extension drives
                                   ///< parser selection (".txt" fallback when absent)
};

/// On-duplicate policy. Parsed from options.on_duplicate.
enum class OnDuplicate { kSkip, kOverwrite, kError };

/// A parsed batch submission request. The HTTP layer
/// (batch_routes) parses the JSON body into this; the service is transport-free
/// so it is unit-testable standalone (mirrors DocumentTaskHandler / §5 step 1-2).
struct BatchRequest {
    std::string namespace_id;
    std::vector<BatchDocument> documents;
    bool async = true;                       ///< topic 3 — always true in V1
    OnDuplicate on_duplicate = OnDuplicate::kSkip;
};

/// A fully-formed HTTP reply: status code + JSON body. A 200 body is the
/// §2.3 partial-success envelope (results[] + meta); a batch-level rejection body
/// is the GEN-Agent error envelope (agent_friendly::ToJson(MakeBatchError(...))).
struct BatchHttpResult {
    int status = 200;
    nlohmann::json body;
};

/// Batch submission limits. Defaults match the detailed
/// design; a Phase 2 anchor exposes these via IGlobalConfig.
struct BatchLimits {
    int max_documents = 100;                          ///< §2.2 — single batch cap
    int64_t max_payload_bytes = 100LL * 1024 * 1024;  ///< §2.2 — total body 100MB
    int64_t max_doc_bytes = 10LL * 1024 * 1024;       ///< §2.2 — per-doc content 10MB
};

/// BatchSubmitService — the POST /documents/batch orchestration,
/// independent of the HTTP transport. Validates the batch envelope (the 4
/// CX_ERR_BATCH_* faults), then per-doc submits through the ITaskSubmitter seam
/// (production = the frozen TaskScheduler), assembling the partial-success
/// response with the GEN-Agent 5-field meta.failed[] schema.
///
/// Standalone (D3): real validation + fan-out + response assembly over a mock
/// ITaskSubmitter, fully unit-tested. DEFERRED → D3.5: (1) the inline
/// content → server-side temp-file materialization that fills SubmitRequest.filepath
/// for the real task/SPC pipeline; (2) real on_duplicate=overwrite cancel-running
/// (write/task coordination); (3) the batch `metric` subsystem wiring
/// into the /metrics registry. Those are live wiring, not this
/// round's scope.
class BatchSubmitService {
public:
    /// @param submitter borrowed per-doc submission seam (must outlive the service)
    /// @param limits    batch envelope limits (§2.2); defaults to the spec values
    explicit BatchSubmitService(ITaskSubmitter* submitter,
                                BatchLimits limits = {});

    /// [D3.5 r2 · P3b] Enable real inline-content materialization: each accepted
    /// doc's content is written to a server-named file under `dir` and that path
    /// becomes the SubmitRequest.filepath the doc-parse worker reads. The dir
    /// is created if absent. When unset (the default), filepath stays "" — the
    /// standalone/mock seam (no real pipeline) keeps working. Call once at wiring
    /// time, passing server::BatchTempDir(data_dir) so the writer and the reapers
    /// (TaskFinalizer release + startup orphan sweep) agree on one location.
    void SetMaterializeDir(std::string dir) { materialize_dir_ = std::move(dir); }

    /// Release seam for an input this service materialized that never acquired an
    /// owner — i.e. the per-doc submit failed, so no task will ever read the file.
    /// (The debounce merge/refresh cases are decided inside the scheduler and are
    /// reported through TaskScheduler::SetUnadoptedInputReleaser instead.) Unset
    /// leaves the file for the orphan sweep, which is the standalone/test default.
    void SetInputReleaser(std::function<void(const std::string&)> fn) {
        input_releaser_ = std::move(fn);
    }

    /// Process a parsed batch. Returns the §2.3 partial-success reply (200) when
    /// the envelope is accepted, or a single GEN-Agent CX_ERR_BATCH_* error reply
    /// (400/413) when the envelope is rejected. Never throws.
    BatchHttpResult Submit(const BatchRequest& req);

    /// The batch-level envelope check (§2.4.1), exposed for tests. Returns the
    /// failing BatchErrorCode + its structured_data when the envelope is invalid;
    /// nullopt when the envelope is acceptable.
    struct EnvelopeError {
        BatchErrorCode code;
        nlohmann::json structured_data;
        std::string message;
    };
    std::optional<EnvelopeError> ValidateEnvelope(const BatchRequest& req) const;

private:
    /// Build the §2.3 results[] item for an accepted doc.
    static nlohmann::json MakeResultItem(const std::string& doc_id,
                                         const std::string& task_id,
                                         const std::string& status);

    /// Map a per-doc submit failure Status to the §2.3 / §2.4.2 GEN-Agent 5-field
    /// meta.failed[] item, reusing the originating Feature's CX_ERR_* token carried
    /// in the Status message (e.g. "CX_ERR_X: detail").
    static nlohmann::json MakeFailureItem(const std::string& doc_id,
                                          const Status& status);

    /// Write `content` to `<materialize_dir_>/<server-minted ULID><ext>` and return
    /// the path, or "" on a write failure (the caller then submits with an empty
    /// filepath, which the pipeline surfaces as a parse failure for that doc).
    ///
    /// [SEC-BATCH-001] No caller-controlled string may reach the path. The basename
    /// is a server-minted ULID. `<ext>` is taken from `filename` and kept verbatim
    /// whenever its SHAPE is valid (lowercase alphanumerics, length-capped),
    /// falling back to ".txt" otherwise: the extension has to survive because
    /// ParseDocument selects the parser from the filepath extension, and filtering
    /// it down to the parser-backed set would silently turn an unsupported-format
    /// failure into a successful plain-text ingest. The doc_id deliberately is NOT
    /// a parameter: it used to form the basename, which let a caller write outside
    /// this directory.
    /// No-op returning "" when materialization is disabled.
    std::string MaterializeContent(const std::string& content,
                                   const std::string& filename) const;

    ITaskSubmitter* submitter_;
    BatchLimits limits_;
    std::string materialize_dir_;  ///< "" = disabled (mock/standalone)
    std::function<void(const std::string&)> input_releaser_;
};

}  // namespace cortrix::server
