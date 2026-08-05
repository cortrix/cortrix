#include "cortrix/memory/memory_transparency.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>

#include "cortrix/id/ulid.h"
#include "cortrix/memory/mem03_metrics.h"

namespace cortrix::memory::transparency {

namespace {

using observability::OperationLogEntry;

/// Current wall clock as Unix epoch milliseconds — transparency stamps revoked_at /
/// deleted_at / last_modified_at as INTEGER epoch ms.
int64_t NowEpochMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

/// Read a metadata_json timestamp key as epoch ms. The extractor writes some timestamps as
/// ISO 8601 strings ("YYYY-MM-DDTHH:MM:SSZ", e.g. created_at / invalidated_at) while
/// Transparency writes its own keys as numeric epoch ms. This tolerant reader returns the
/// number directly, or parses the ISO form to epoch ms, or nullopt when the key is
/// absent / unparseable.
std::optional<int64_t> ReadTimestampMs(const nlohmann::json& meta, const char* key) {
    if (!meta.is_object() || !meta.contains(key)) return std::nullopt;
    const nlohmann::json& v = meta.at(key);
    if (v.is_number_integer()) return v.get<int64_t>();
    if (v.is_number_float()) return static_cast<int64_t>(v.get<double>());
    if (v.is_string()) {
        const std::string s = v.get<std::string>();
        std::tm tm{};
        // Parse the canonical UTC form the extractor emits. sscanf avoids strptime portability gaps.
        int y = 0, mo = 0, d = 0, h = 0, mi = 0, se = 0;
        if (std::sscanf(s.c_str(), "%d-%d-%dT%d:%d:%dZ", &y, &mo, &d, &h, &mi, &se) == 6) {
            tm.tm_year = y - 1900;
            tm.tm_mon = mo - 1;
            tm.tm_mday = d;
            tm.tm_hour = h;
            tm.tm_min = mi;
            tm.tm_sec = se;
#if defined(_WIN32)
            std::time_t t = _mkgmtime(&tm);
#else
            std::time_t t = timegm(&tm);
#endif
            if (t != static_cast<std::time_t>(-1)) {
                return static_cast<int64_t>(t) * 1000;
            }
        }
        return std::nullopt;
    }
    return std::nullopt;
}

/// Read a metadata_json string key, or nullopt when absent / not a non-empty string.
std::optional<std::string> ReadOptString(const nlohmann::json& meta, const char* key) {
    if (!meta.is_object() || !meta.contains(key)) return std::nullopt;
    const nlohmann::json& v = meta.at(key);
    if (!v.is_string()) return std::nullopt;
    std::string s = v.get<std::string>();
    if (s.empty()) return std::nullopt;
    return s;
}

/// Read a metadata_json string key, or "" when absent.
std::string ReadString(const nlohmann::json& meta, const char* key) {
    if (!meta.is_object() || !meta.contains(key) || !meta.at(key).is_string()) return "";
    return meta.at(key).get<std::string>();
}

bool IsValidMemoryType(const std::string& t) {
    return t == "fact" || t == "preference" || t == "event";
}

/// Map a Mem03ErrorCode to its metric ErrorCodeLabel (1:1, same order).
Mem03Metrics::ErrorCodeLabel MetricLabel(Mem03ErrorCode code) {
    switch (code) {
        case Mem03ErrorCode::kMemoryNotFound:     return Mem03Metrics::ErrorCodeLabel::kMemoryNotFound;
        case Mem03ErrorCode::kUserMismatch:       return Mem03Metrics::ErrorCodeLabel::kUserMismatch;
        case Mem03ErrorCode::kAlreadyInvalidated: return Mem03Metrics::ErrorCodeLabel::kAlreadyInvalidated;
        case Mem03ErrorCode::kInvalidateFailed:   return Mem03Metrics::ErrorCodeLabel::kInvalidateFailed;
        case Mem03ErrorCode::kQuota:              return Mem03Metrics::ErrorCodeLabel::kQuota;
    }
    return Mem03Metrics::ErrorCodeLabel::kInvalidateFailed;
}

/// Mask the second half of a user_id for the USER_MISMATCH structured_data (the
/// spec example "user_y****"): keep up to the first 6 chars, then "****".
std::string MaskUserId(const std::string& uid) {
    constexpr size_t kKeep = 6;
    if (uid.size() <= kKeep) return uid + "****";
    return uid.substr(0, kKeep) + "****";
}

/// Build the operation_log entry for a memory operation. triggered_by is encoded in
/// the CE-visible `summary` field (the user-view track) — the CE OperationLogEntry has
/// no details_json (that is the Ent extension table, §5.1 note), so the real
/// details_json.triggered_by lands in D3.5; the standalone CE record carries it in
/// summary so the audit trail still distinguishes the 4 paths (§5.4).
OperationLogEntry MakeOpLogEntry(const std::string& action,
                                 const std::string& user_id,
                                 const std::string& ns_id,
                                 const std::string& memory_id,
                                 TriggeredBy triggered_by,
                                 const std::string& extra_summary = "") {
    OperationLogEntry e;
    e.user_id = user_id;
    e.action = action;
    if (!ns_id.empty()) e.namespace_id = ns_id;
    e.resource_type = "memory";
    e.resource_id = memory_id;
    std::string summary = std::string("triggered_by=") + ToString(triggered_by);
    if (!extra_summary.empty()) summary += "; " + extra_summary;
    e.summary = summary;
    return e;
}

}  // namespace

const char* ToString(TriggeredBy triggered_by) {
    switch (triggered_by) {
        case TriggeredBy::kUserManual: return "user_manual";
        case TriggeredBy::kUserEdit:   return "user_edit";
        case TriggeredBy::kAdminRevoke: return "admin_revoke";
        case TriggeredBy::kLlmAuto:    return "llm_auto";
    }
    return "unknown";
}

MemoryTransparency::MemoryTransparency(
    std::shared_ptr<IMemoryBlockLister> lister,
    std::shared_ptr<IMemoryBlockStore> block_store,
    std::shared_ptr<observability::IOperationLogger> op_logger)
    : lister_(std::move(lister)),
      block_store_(std::move(block_store)),
      op_logger_(std::move(op_logger)) {}

bool MemoryTransparency::IsEffectiveStatus(const MemoryBlockRecord& rec) {
    // A memory's effective (valid) state aligns with the extractor's write side = active;
    // tentative is also a non-invalidated state the user still "has". invalidated is
    // the only state hidden by include_invalidated=false.
    const std::string status = ReadString(rec.metadata_json, "status");
    return status == ToString(MemoryStatus::kActive) ||
           status == ToString(MemoryStatus::kTentative);
}

MemoryListItem MemoryTransparency::ProjectListItem(const MemoryBlockRecord& rec,
                                                   bool explain) {
    const nlohmann::json& m = rec.metadata_json;
    MemoryListItem item;
    // A-class.
    item.memory_id = rec.block_id;
    item.content = rec.content;
    item.memory_type = ReadString(m, "memory_type");
    item.status = ReadString(m, "status");
    // user_id: prefer the struct field, fall back to the metadata key.
    item.user_id = !rec.user_id.empty() ? rec.user_id : ReadString(m, "user_id");
    // The extractor stamps the extraction time under "created_at"; honor transparency's own
    // "extracted_at" key too if present.
    item.extracted_at = ReadTimestampMs(m, "extracted_at");
    if (!item.extracted_at.has_value()) item.extracted_at = ReadTimestampMs(m, "created_at");
    item.invalidated_at = ReadTimestampMs(m, "invalidated_at");
    item.revoked_at = ReadTimestampMs(m, "revoked_at");
    // B-class (explain only).
    if (explain) {
        item.invalidated_by_block_id = ReadOptString(m, "invalidated_by_block_id");
        item.extraction_method = ReadOptString(m, "extraction_method");
        item.source_session_id = ReadOptString(m, "source_session_id");
        item.source_interaction_id = ReadOptString(m, "source_interaction_id");
    }
    return item;
}

Result<MemoryBlockRecord> MemoryTransparency::FetchOwnedMemory(
    const std::string& memory_id, const std::string& session_user_id) {
    Result<MemoryBlockRecord> r = block_store_->GetMemoryBlock(memory_id);
    if (!r.ok()) {
        // Absent (or store NotFound) → 404 mask. An attacker cannot tell "absent" from
        // "not yours".
        Mem03Metrics::Instance().RecordInvalidInput(
            MetricLabel(Mem03ErrorCode::kMemoryNotFound));
        return Mem03Status(Mem03ErrorCode::kMemoryNotFound, memory_id);
    }
    MemoryBlockRecord rec = r.value();
    const std::string owner =
        !rec.user_id.empty() ? rec.user_id : ReadString(rec.metadata_json, "user_id");
    if (owner != session_user_id) {
        // Cross-user hit: count the block + return the SAME 404 (L1.bis anti-enumeration mask).
        Mem03Metrics::Instance().RecordCrossUserBlocked();
        Mem03Metrics::Instance().RecordInvalidInput(
            MetricLabel(Mem03ErrorCode::kMemoryNotFound));
        return Mem03Status(Mem03ErrorCode::kMemoryNotFound, memory_id);
    }
    return rec;
}

Result<MemoryListResponse> MemoryTransparency::List(const MemoryListFilter& filter,
                                                    const std::string& session_user_id,
                                                    bool explain,
                                                    const observability::TraceContext* ctx) {
    (void)ctx;  // trace propagation is a D3.5 wiring concern (seam takes no ctx)
    const auto t0 = std::chrono::steady_clock::now();
    auto& metrics = Mem03Metrics::Instance();

    // L1: filter.user_id MUST equal the session user (no cross-user list).
    if (filter.user_id != session_user_id) {
        metrics.RecordOp(Mem03Metrics::Op::kList, Mem03Metrics::OpStatus::kError);
        metrics.RecordInvalidInput(MetricLabel(Mem03ErrorCode::kUserMismatch));
        return Mem03Status(Mem03ErrorCode::kUserMismatch,
                           "list user_id must match session user");
    }

    Result<std::vector<MemoryBlockRecord>> listed = lister_->ListByUser(filter.user_id);
    if (!listed.ok()) {
        metrics.RecordOp(Mem03Metrics::Op::kList, Mem03Metrics::OpStatus::kError);
        return listed.status();
    }

    // Apply status + memory_type predicates.
    std::vector<MemoryBlockRecord> matched;
    matched.reserve(listed.value().size());
    for (const MemoryBlockRecord& rec : listed.value()) {
        if (!filter.include_invalidated && !IsEffectiveStatus(rec)) continue;
        if (filter.memory_type.has_value() &&
            ReadString(rec.metadata_json, "memory_type") != *filter.memory_type) {
            continue;
        }
        matched.push_back(rec);
    }

    MemoryListResponse resp;
    resp.total = static_cast<int64_t>(matched.size());
    resp.page = filter.page < 0 ? 0 : filter.page;
    int page_size = filter.page_size;
    if (page_size <= 0) page_size = 50;
    if (page_size > kMaxPageSize) {
        page_size = kMaxPageSize;
        resp.warnings.push_back("page_size clamped to maximum 200");
    }
    resp.page_size = page_size;

    // Pagination over the matched set.
    const size_t start = static_cast<size_t>(resp.page) * static_cast<size_t>(page_size);
    for (size_t i = start; i < matched.size() && i < start + static_cast<size_t>(page_size); ++i) {
        resp.memories.push_back(ProjectListItem(matched[i], explain));
    }

    const auto t1 = std::chrono::steady_clock::now();
    resp.latency_ms = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count());
    metrics.RecordOp(Mem03Metrics::Op::kList, Mem03Metrics::OpStatus::kSuccess);
    metrics.ObserveOpLatency(Mem03Metrics::Op::kList, resp.latency_ms);
    return resp;
}

Result<std::string> MemoryTransparency::Create(const MemoryCreateRequest& req,
                                               const std::string& session_user_id,
                                               const observability::TraceContext* ctx) {
    (void)ctx;
    auto& metrics = Mem03Metrics::Instance();

    // L1: the create must be for the session user.
    if (req.user_id != session_user_id) {
        metrics.RecordOp(Mem03Metrics::Op::kCreate, Mem03Metrics::OpStatus::kError);
        metrics.RecordInvalidInput(MetricLabel(Mem03ErrorCode::kUserMismatch));
        return Mem03Status(Mem03ErrorCode::kUserMismatch,
                           "create user_id must match session user");
    }
    // issue-8 error 3: memory_type ∈ {fact,preference,event}.
    if (!IsValidMemoryType(req.memory_type)) {
        metrics.RecordOp(Mem03Metrics::Op::kCreate, Mem03Metrics::OpStatus::kError);
        // CRUD INVALID_TYPE folds onto the boundary as InvalidArgument; the metric
        // tracks it under the closest registered code's "invalid input" family.
        metrics.RecordInvalidInput(MetricLabel(Mem03ErrorCode::kInvalidateFailed));
        return Status::InvalidArgument(
            "CX_ERR_MEM03_INVALID_TYPE: memory_type must be fact|preference|event");
    }
    // issue-8 error 4: content length ≤ 2000.
    if (static_cast<int>(req.content.size()) > kMaxContentLength) {
        metrics.RecordOp(Mem03Metrics::Op::kCreate, Mem03Metrics::OpStatus::kError);
        metrics.RecordInvalidInput(MetricLabel(Mem03ErrorCode::kInvalidateFailed));
        return Status::InvalidArgument(
            "CX_ERR_MEM03_CONTENT_TOO_LONG: content exceeds 2000 chars");
    }

    const int64_t now_ms = NowEpochMs();
    MemoryBlockRecord rec;
    rec.block_id = id::GenerateUlid(now_ms);  // reuse the chunker's ULID minting (no other id minting)
    rec.user_id = req.user_id;
    rec.content = req.content;
    // issue-3 A: POST path writes directly, NO LLM — extraction_method="explicit".
    rec.metadata_json = {
        {"memory_type", req.memory_type},
        {"status", ToString(MemoryStatus::kActive)},
        {"extraction_method", "explicit"},
        {"user_id", req.user_id},
        {"extracted_at", now_ms},
        {"last_modified_at", now_ms},
    };

    Result<std::string> ins = block_store_->InsertMemoryBlock(rec);
    if (!ins.ok()) {
        metrics.RecordOp(Mem03Metrics::Op::kCreate, Mem03Metrics::OpStatus::kError);
        return ins.status();
    }

    // audit (memory_create). Creation is not an invalidation, so triggered_by is
    // the manual user-create path; encoded in summary (CE track).
    if (op_logger_) op_logger_->Log(MakeOpLogEntry("memory_create", req.user_id, rec.ns_id, rec.block_id,
                                   TriggeredBy::kUserManual,
                                   "extraction_method=explicit"));

    metrics.RecordOp(Mem03Metrics::Op::kCreate, Mem03Metrics::OpStatus::kSuccess);
    return rec.block_id;
}

Result<MemoryEditResult> MemoryTransparency::Edit(const MemoryEditRequest& req,
                                                  const std::string& session_user_id,
                                                  const observability::TraceContext* ctx) {
    (void)ctx;
    auto& metrics = Mem03Metrics::Instance();

    // 1. Fetch + ownership (L1.bis 404 mask).
    Result<MemoryBlockRecord> owned = FetchOwnedMemory(req.memory_id, session_user_id);
    if (!owned.ok()) {
        metrics.RecordOp(Mem03Metrics::Op::kEdit, Mem03Metrics::OpStatus::kError);
        return owned.status();
    }
    MemoryBlockRecord old_block = owned.value();
    if (!old_block.metadata_json.is_object()) {
        old_block.metadata_json = nlohmann::json::object();
    }

    // Validate the new type (when provided).
    const std::string new_type =
        req.new_memory_type.has_value() ? *req.new_memory_type
                                        : ReadString(old_block.metadata_json, "memory_type");
    if (req.new_memory_type.has_value() && !IsValidMemoryType(new_type)) {
        metrics.RecordOp(Mem03Metrics::Op::kEdit, Mem03Metrics::OpStatus::kError);
        metrics.RecordInvalidInput(MetricLabel(Mem03ErrorCode::kInvalidateFailed));
        return Status::InvalidArgument(
            "CX_ERR_MEM03_INVALID_TYPE: memory_type must be fact|preference|event");
    }
    const std::string new_content =
        req.new_content.has_value() ? *req.new_content : old_block.content;
    if (static_cast<int>(new_content.size()) > kMaxContentLength) {
        metrics.RecordOp(Mem03Metrics::Op::kEdit, Mem03Metrics::OpStatus::kError);
        metrics.RecordInvalidInput(MetricLabel(Mem03ErrorCode::kInvalidateFailed));
        return Status::InvalidArgument(
            "CX_ERR_MEM03_CONTENT_TOO_LONG: content exceeds 2000 chars");
    }

    // 2. Optimistic lock: expected_modified_at must match last_modified_at (issue-8 #5).
    if (req.expected_modified_at.has_value()) {
        const std::optional<int64_t> cur =
            ReadTimestampMs(old_block.metadata_json, "last_modified_at");
        if (!cur.has_value() || *cur != *req.expected_modified_at) {
            metrics.RecordOp(Mem03Metrics::Op::kEdit, Mem03Metrics::OpStatus::kError);
            metrics.RecordEditConflict();
            return Status(
                StatusCode::kUnavailable,
                "CX_ERR_MEM03_EDIT_CONCURRENT: last_modified_at mismatch (expected " +
                    std::to_string(*req.expected_modified_at) + ", actual " +
                    (cur.has_value() ? std::to_string(*cur) : std::string("none")) + ")");
        }
    }

    const int64_t now_ms = NowEpochMs();

    // 3. INSERT the new block (extraction_method="user_edit", issue-2 B).
    MemoryBlockRecord new_block;
    new_block.block_id = id::GenerateUlid(now_ms);
    new_block.ns_id = old_block.ns_id;
    new_block.user_id = old_block.user_id;
    new_block.content = new_content;
    new_block.metadata_json = {
        {"memory_type", new_type},
        {"status", ToString(MemoryStatus::kActive)},
        {"extraction_method", "user_edit"},  // issue-2 B new value (free string, no enum change)
        {"user_id", old_block.user_id},
        {"extracted_at", now_ms},
        {"last_modified_at", now_ms},
    };
    // Carry source provenance forward when present (so explain still links the lineage).
    if (auto ss = ReadOptString(old_block.metadata_json, "source_session_id"))
        new_block.metadata_json["source_session_id"] = *ss;
    if (auto si = ReadOptString(old_block.metadata_json, "source_interaction_id"))
        new_block.metadata_json["source_interaction_id"] = *si;

    Result<std::string> ins = block_store_->InsertMemoryBlock(new_block);
    if (!ins.ok()) {
        metrics.RecordOp(Mem03Metrics::Op::kEdit, Mem03Metrics::OpStatus::kError);
        metrics.RecordInvalidInput(MetricLabel(Mem03ErrorCode::kInvalidateFailed));
        return Mem03Status(Mem03ErrorCode::kInvalidateFailed,
                           "insert new edited block failed");
    }

    // 4. INVALIDATE the old block (status=invalidated + invalidated_by_block_id).
    old_block.metadata_json["status"] = ToString(MemoryStatus::kInvalidated);
    old_block.metadata_json["invalidated_by_block_id"] = new_block.block_id;
    old_block.metadata_json["invalidated_at"] = now_ms;
    old_block.metadata_json["last_modified_at"] = now_ms;
    old_block.metadata_json["invalidation_triggered_by"] = ToString(TriggeredBy::kUserEdit);
    Status us = block_store_->UpdateMemoryBlock(old_block);
    if (!us.ok()) {
        metrics.RecordOp(Mem03Metrics::Op::kEdit, Mem03Metrics::OpStatus::kError);
        metrics.RecordInvalidInput(MetricLabel(Mem03ErrorCode::kInvalidateFailed));
        return Mem03Status(Mem03ErrorCode::kInvalidateFailed,
                           "invalidate old block failed");
    }

    // 5. audit: memory_edit + the cascaded memory_invalidate of the old block
    //    (§4.2 step 5 — two records; triggered_by=user_edit distinguishes the cascade).
    if (op_logger_) op_logger_->Log(MakeOpLogEntry("memory_edit", session_user_id, new_block.ns_id,
                                   new_block.block_id, TriggeredBy::kUserEdit,
                                   "old_memory_id=" + old_block.block_id));
    if (op_logger_) op_logger_->Log(MakeOpLogEntry("memory_invalidate", session_user_id,
                                   old_block.ns_id, old_block.block_id,
                                   TriggeredBy::kUserEdit, "cascaded_from=memory_edit"));

    metrics.RecordOp(Mem03Metrics::Op::kEdit, Mem03Metrics::OpStatus::kSuccess);
    MemoryEditResult result;
    result.new_memory_id = new_block.block_id;
    result.old_memory_id = old_block.block_id;
    return result;
}

Status MemoryTransparency::Delete(const std::string& memory_id,
                                  const std::string& session_user_id,
                                  const observability::TraceContext* ctx) {
    (void)ctx;
    auto& metrics = Mem03Metrics::Instance();

    // 1. Fetch + ownership (404 mask).
    Result<MemoryBlockRecord> owned = FetchOwnedMemory(memory_id, session_user_id);
    if (!owned.ok()) {
        metrics.RecordOp(Mem03Metrics::Op::kInvalidate, Mem03Metrics::OpStatus::kError);
        return owned.status();
    }
    MemoryBlockRecord rec = owned.value();
    if (!rec.metadata_json.is_object()) {
        rec.metadata_json = nlohmann::json::object();
    }

    // 2. Idempotent: already invalidated → return Ok (no error, §4.2 step 2).
    if (ReadString(rec.metadata_json, "status") == ToString(MemoryStatus::kInvalidated)) {
        metrics.RecordOp(Mem03Metrics::Op::kInvalidate, Mem03Metrics::OpStatus::kSuccess);
        return Status::Ok();
    }

    // 3. Soft delete: status=invalidated + deleted_by_user_id + deleted_at (§6.1 keys).
    const int64_t now_ms = NowEpochMs();
    rec.metadata_json["status"] = ToString(MemoryStatus::kInvalidated);
    rec.metadata_json["deleted_by_user_id"] = session_user_id;
    rec.metadata_json["deleted_at"] = now_ms;
    rec.metadata_json["invalidated_at"] = now_ms;
    rec.metadata_json["last_modified_at"] = now_ms;
    rec.metadata_json["invalidation_triggered_by"] = ToString(TriggeredBy::kUserManual);
    Status us = block_store_->UpdateMemoryBlock(rec);
    if (!us.ok()) {
        metrics.RecordOp(Mem03Metrics::Op::kInvalidate, Mem03Metrics::OpStatus::kError);
        metrics.RecordInvalidInput(MetricLabel(Mem03ErrorCode::kInvalidateFailed));
        return Mem03Status(Mem03ErrorCode::kInvalidateFailed, "soft-delete update failed");
    }

    // 4. audit: memory_invalidate, triggered_by=user_manual (§4.2 step 4).
    if (op_logger_) op_logger_->Log(MakeOpLogEntry("memory_invalidate", session_user_id, rec.ns_id,
                                   memory_id, TriggeredBy::kUserManual));

    metrics.RecordOp(Mem03Metrics::Op::kInvalidate, Mem03Metrics::OpStatus::kSuccess);
    return Status::Ok();
}

}  // namespace cortrix::memory::transparency
