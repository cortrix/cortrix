#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cortrix/server/batch_error.h"
#include "cortrix/server/i_task_submitter.h"

namespace cortrix::server {

/// One document of a parsed batch request (TD-F42-BULK §2.2 documents[] item).
struct BatchDocument {
    std::string doc_id;            ///< client-provided, unique within the batch
    std::string content;           ///< inline content (url / file_path are Phase 2)
    std::string metadata_json;     ///< optional raw JSON object string ("" if absent)
    std::string filename;          ///< optional client filename; its extension drives
                                   ///< parser selection (".txt" fallback when absent)
};

/// On-duplicate policy (TD-F42-BULK §2.2 topic 2). Parsed from options.on_duplicate.
enum class OnDuplicate { kSkip, kOverwrite, kError };

/// A parsed batch submission request (TD-F42-BULK §2.2). The HTTP layer
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

/// Batch submission limits (TD-F42-BULK §2.2 topic 1). Defaults match the detailed
/// design; a Phase 2 anchor exposes these via IGlobalConfig (TD-F42-BULK-CONFIG).
struct BatchLimits {
    int max_documents = 100;                          ///< §2.2 — single batch cap
    int64_t max_payload_bytes = 100LL * 1024 * 1024;  ///< §2.2 — total body 100MB
    int64_t max_doc_bytes = 10LL * 1024 * 1024;       ///< §2.2 — per-doc content 10MB
};

/// BatchSubmitService — the POST /documents/batch orchestration (TD-F42-BULK
/// §3), independent of the HTTP transport. Validates the batch envelope (the 4
/// CX_ERR_BATCH_* faults), then per-doc submits through the ITaskSubmitter seam
/// (production = the frozen F42 TaskScheduler), assembling the §2.3 partial-success
/// response with the GEN-Agent 5-field meta.failed[] schema.
///
/// Standalone (D3): real validation + fan-out + response assembly over a mock
/// ITaskSubmitter, fully unit-tested. DEFERRED → D3.5: (1) the inline
/// content → server-side temp-file materialization that fills SubmitRequest.filepath
/// for the real F42/SPC pipeline; (2) real on_duplicate=overwrite cancel-running
/// (TD-F42-BULK-2 F25/F42 coordination); (3) the batch `metric` subsystem wiring
/// into the F24 /metrics registry (§4.bis). Those are cross-Feature live wiring, not this
/// round's scope.
class BatchSubmitService {
public:
    /// @param submitter borrowed per-doc submission seam (must outlive the service)
    /// @param limits    batch envelope limits (§2.2); defaults to the spec values
    explicit BatchSubmitService(ITaskSubmitter* submitter,
                                BatchLimits limits = {});

    /// [D3.5 r2 · P3b] Enable real inline-content materialization: each accepted
    /// doc's content is written to `<dir>/<doc_id>` and that path becomes the
    /// SubmitRequest.filepath the F42 doc-parse worker reads. The dir is created if
    /// absent. When unset (the default), filepath stays "" — the standalone/mock
    /// seam (no real pipeline) keeps working. Call once at wiring time.
    void SetMaterializeDir(std::string dir) { materialize_dir_ = std::move(dir); }

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

    /// Write `content` to `<materialize_dir_>/<doc_id><ext>` and return the path, or
    /// "" on a write failure (the caller then submits with an empty filepath, which
    /// the pipeline surfaces as a parse failure for that doc). `<ext>` is taken from
    /// `filename`'s extension so the F42 doc-parse worker
    /// (DocumentParserFactory::ParseDocument selects a parser by the filepath
    /// extension); it falls back to ".txt" (plain text) when `filename` carries no
    /// usable extension. The on-disk name keys on doc_id (unique within the batch) so
    /// duplicate client filenames cannot collide. No-op returning "" when
    /// materialization is disabled.
    std::string MaterializeContent(const std::string& doc_id,
                                   const std::string& content,
                                   const std::string& filename) const;

    ITaskSubmitter* submitter_;
    BatchLimits limits_;
    std::string materialize_dir_;  ///< "" = disabled (mock/standalone)
};

}  // namespace cortrix::server
