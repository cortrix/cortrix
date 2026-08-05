#include <cstdint>
#include "cortrix/memory/memory_block_adapter.h"

#include <sqlite3.h>

#include <algorithm>

#include <nlohmann/json.hpp>

#include "cortrix/common/block_header.h"
#include "cortrix/common/block_types.h"
#include "cortrix/common/data_types.h"
#include "cortrix/id/hash.h"
#include "cortrix/logging/logging.h"
#include "cortrix/spc/onnx_embedder.h"
#include "cortrix/store/cortrix_store.h"
#include "cortrix/store/iindex.h"

namespace cortrix::memory {

namespace {

// A memory fact is a blocks row (block_type=kBlockMemory). The extraction ULID is the
// business id; the uint64 storage key is HashChildIdToBlockId(ulid) (same scheme the
// child blocks use). The ULID is preserved in metadata_json.block_id so reads can
// echo it back.
uint64_t MemBlockId(const std::string& ulid) { return id::HashChildIdToBlockId(ulid); }

// Memory facts are blocks, and the blocks table FK-references documents(doc_id). All
// extracted memories in a namespace share one stable synthetic "memory document" so
// the FK is satisfied without tying a fact to a real ingested doc. Idempotent: created
// once per namespace store, reused thereafter.
constexpr const char* kMemoryDocId = "__cortrix_memory__";

void EnsureMemoryDoc(CortrixStore& store) {
    CortrixDoc existing;
    if (store.doc_get(kMemoryDocId, existing) == 0) return;  // already present
    CortrixDoc doc;
    doc.doc_id = kMemoryDocId;
    doc.source_type = "memory";
    doc.source_path = kMemoryDocId;
    doc.status = DocStatus::kReady;
    doc.processing_level = 3;
    doc.title = "Cortrix Memory Facts";
    store.doc_create(doc);  // best-effort; a concurrent create races to the same id
}

// Build a MemoryBlockRecord from a stored CortrixBlock (content_text + metadata_json).
// The ULID id is read back from metadata_json.block_id (fallback: empty).
MemoryBlockRecord ToRecord(const CortrixBlock& b) {
    MemoryBlockRecord rec;
    rec.content = b.content_text;
    auto j = nlohmann::json::parse(b.metadata_json, nullptr, /*allow_exceptions=*/false);
    if (!j.is_discarded() && j.is_object()) {
        rec.metadata_json = j;
        if (j.contains("block_id") && j["block_id"].is_string())
            rec.block_id = j["block_id"].get<std::string>();
        if (j.contains("user_id") && j["user_id"].is_string())
            rec.user_id = j["user_id"].get<std::string>();
    } else {
        rec.metadata_json = nlohmann::json::object();
    }
    return rec;
}

std::string JsonStr(const nlohmann::json& j, const char* key) {
    if (j.contains(key) && j[key].is_string()) return j[key].get<std::string>();
    return "";
}

MemoryType ParseTypeStr(const std::string& s) { return ParseMemoryType(s); }

}  // namespace

// ---------------------------------------------------------------------------
// MemoryBlockAdapter
// ---------------------------------------------------------------------------

MemoryBlockAdapter::MemoryBlockAdapter(CortrixStore& store, OnnxEmbedder* embedder,
                                       store::IIndex* vec_index)
    : store_(store), embedder_(embedder), vec_index_(vec_index) {}

Result<std::string> MemoryBlockAdapter::InsertMemoryBlock(const MemoryBlockRecord& block) {
    // The extracted metadata already carries block_id/user_id; ensure block_id is present
    // so reads can echo the ULID. content_text = the fact text (semantically searched).
    nlohmann::json meta = block.metadata_json.is_object() ? block.metadata_json
                                                          : nlohmann::json::object();
    if (!meta.contains("block_id")) meta["block_id"] = block.block_id;
    if (!meta.contains("user_id") && !block.user_id.empty()) meta["user_id"] = block.user_id;

    const uint64_t bid = MemBlockId(block.block_id);

    // Embed the content so the fact is searchable (memories share the P-HNSW
    // index). Degrade gracefully when no embedder/index is wired (the row is still
    // stored, just not vector-indexed).
    EmbeddingResult emb;
    bool have_vec = false;
    if (embedder_) {
        std::vector<EmbeddingResult> out;
        if (embedder_->EmbedBatch({block.content}, &out).ok() && !out.empty()) {
            emb = std::move(out[0]);
            have_vec = !emb.vector.empty();
        }
    }

    EnsureMemoryDoc(store_);  // FK target for the memory block (blocks.doc_id → documents)
    CortrixBlock cb;
    cb.block_id = bid;
    cb.doc_id = kMemoryDocId;  // shared synthetic memory document (satisfies the FK)
    cb.chunk_index = 0;
    cb.block_type = static_cast<int>(kBlockMemory);
    cb.processing_level = static_cast<int>(kLevelL3);
    cb.content_text = block.content;
    cb.metadata_json = meta.dump();
    cb.data = BlockBuild(kBlockMemory, kLevelL3, block.content, cb.metadata_json, "",
                         have_vec ? static_cast<uint16_t>(emb.dim) : 0, 0, /*flags_ext=*/0);

    if (int rc = store_.block_insert(cb); rc != 0) {
        return Result<std::string>(
            Status::Internal("CX_ERR_MEMEXTRACT_STORE: block_insert failed for memory block"));
    }
    if (have_vec && vec_index_) {
        std::vector<std::pair<const float*, uint64_t>> pts{{emb.vector.data(), bid}};
        Status av = vec_index_->AddPoints(pts);
        if (!av.ok()) {
            CORTRIX_LOG_WARN("mem02",
                "memory vector index add failed for block {}: {}", block.block_id,
                av.message());
        }
    }
    return Result<std::string>(block.block_id);
}

Status MemoryBlockAdapter::UpdateMemoryBlock(const MemoryBlockRecord& block) {
    // Re-fetch the stored row to keep content_text, overwrite metadata_json (the
    // caller mutates status / invalidation / mention_count). Direct UPDATE on the
    // blocks.metadata_json column keyed by the hashed block_id.
    sqlite3* db = store_.db_handle();
    if (!db) return Status::Ok();  // test fake store — no-op
    nlohmann::json meta = block.metadata_json.is_object() ? block.metadata_json
                                                          : nlohmann::json::object();
    if (!meta.contains("block_id")) meta["block_id"] = block.block_id;
    const std::string meta_str = meta.dump();
    const uint64_t bid = MemBlockId(block.block_id);

    sqlite3_stmt* stmt = nullptr;
    const char* sql = "UPDATE blocks SET metadata_json=?1 WHERE block_id=?2";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return Status::Internal("CX_ERR_MEMEXTRACT_STORE: prepare update memory block");
    sqlite3_bind_text(stmt, 1, meta_str.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(bid));
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE)
        return Status::Internal("CX_ERR_MEMEXTRACT_STORE: update memory block failed");
    return Status::Ok();
}

Result<MemoryBlockRecord> MemoryBlockAdapter::GetMemoryBlock(const std::string& block_id) {
    CortrixBlock cb;
    if (store_.block_get(MemBlockId(block_id), cb) != 0) {
        return Result<MemoryBlockRecord>(
            Status::NotFound("CX_ERR_MEMEXTRACT_STORE: memory block not found"));
    }
    MemoryBlockRecord rec = ToRecord(cb);
    if (rec.block_id.empty()) rec.block_id = block_id;
    return Result<MemoryBlockRecord>(std::move(rec));
}

Result<std::vector<MemoryBlockRecord>> MemoryBlockAdapter::ListByUser(
    const std::string& user_id) {
    std::vector<MemoryBlockRecord> out;
    sqlite3* db = store_.db_handle();
    if (!db) return Result<std::vector<MemoryBlockRecord>>(std::move(out));

    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT content_text, metadata_json FROM blocks "
        "WHERE block_type=?1 "
        "AND json_extract(metadata_json,'$.user_id')=?2 "
        "AND json_extract(metadata_json,'$.memory_type') IS NOT NULL "
        "ORDER BY json_extract(metadata_json,'$.created_at') DESC";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return Result<std::vector<MemoryBlockRecord>>(
            Status::Internal("CX_ERR_MEMORY_STORE: prepare list by user"));
    }
    sqlite3_bind_int(stmt, 1, static_cast<int>(kBlockMemory));
    sqlite3_bind_text(stmt, 2, user_id.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        CortrixBlock cb;
        const unsigned char* ct = sqlite3_column_text(stmt, 0);
        const unsigned char* mj = sqlite3_column_text(stmt, 1);
        cb.content_text = ct ? reinterpret_cast<const char*>(ct) : "";
        cb.metadata_json = mj ? reinterpret_cast<const char*>(mj) : "{}";
        out.push_back(ToRecord(cb));
    }
    sqlite3_finalize(stmt);
    return Result<std::vector<MemoryBlockRecord>>(std::move(out));
}

// ---------------------------------------------------------------------------
// MemoryContradictionAdapter
// ---------------------------------------------------------------------------

MemoryContradictionAdapter::MemoryContradictionAdapter(CortrixStore& store)
    : store_(store) {}

std::vector<CandidateBlock> MemoryContradictionAdapter::FindCandidates(
    const std::string& ns, const std::string& content, int top_k) {
    (void)ns;
    (void)content;
    std::vector<CandidateBlock> out;
    sqlite3* db = store_.db_handle();
    if (!db) return out;
    // Active fact/preference memories are the contradiction candidates; the LLM judge
    // decides actual conflict. Newest first, capped at top_k.
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT metadata_json, content_text FROM blocks "
        "WHERE block_type=?1 "
        "AND json_extract(metadata_json,'$.status')='active' "
        "AND json_extract(metadata_json,'$.memory_type') IN ('fact','preference') "
        "ORDER BY json_extract(metadata_json,'$.created_at') DESC LIMIT ?2";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return out;
    sqlite3_bind_int(stmt, 1, static_cast<int>(kBlockMemory));
    sqlite3_bind_int(stmt, 2, top_k > 0 ? top_k : 5);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char* mj = sqlite3_column_text(stmt, 0);
        const unsigned char* ct = sqlite3_column_text(stmt, 1);
        auto j = nlohmann::json::parse(mj ? reinterpret_cast<const char*>(mj) : "{}",
                                       nullptr, false);
        if (j.is_discarded() || !j.is_object()) continue;
        CandidateBlock c;
        c.block_id = JsonStr(j, "block_id");
        c.content = ct ? reinterpret_cast<const char*>(ct) : "";
        c.type = ParseTypeStr(JsonStr(j, "memory_type"));
        c.score = 0.0;
        out.push_back(std::move(c));
    }
    sqlite3_finalize(stmt);
    return out;
}

std::optional<CandidateBlock> MemoryContradictionAdapter::FindMatchingPreference(
    const std::string& ns, const std::string& content) {
    (void)ns;
    // D8 first-mention check: a same-text active preference is a repeat mention. Exact
    // content match is the conservative Phase-1 rule (semantic dedup = Phase 2).
    sqlite3* db = store_.db_handle();
    if (!db) return std::nullopt;
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT metadata_json, content_text FROM blocks "
        "WHERE block_type=?1 "
        "AND json_extract(metadata_json,'$.status')='active' "
        "AND json_extract(metadata_json,'$.memory_type')='preference' "
        "AND content_text=?2 LIMIT 1";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return std::nullopt;
    sqlite3_bind_int(stmt, 1, static_cast<int>(kBlockMemory));
    sqlite3_bind_text(stmt, 2, content.c_str(), -1, SQLITE_TRANSIENT);
    std::optional<CandidateBlock> out;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char* mj = sqlite3_column_text(stmt, 0);
        const unsigned char* ct = sqlite3_column_text(stmt, 1);
        auto j = nlohmann::json::parse(mj ? reinterpret_cast<const char*>(mj) : "{}",
                                       nullptr, false);
        if (!j.is_discarded() && j.is_object()) {
            CandidateBlock c;
            c.block_id = JsonStr(j, "block_id");
            c.content = ct ? reinterpret_cast<const char*>(ct) : "";
            c.type = MemoryType::kPreference;
            out = std::move(c);
        }
    }
    sqlite3_finalize(stmt);
    return out;
}

// ---------------------------------------------------------------------------
// QueryUserFacts (chat-path stable interface)
// ---------------------------------------------------------------------------

Result<std::vector<UserFact>> QueryUserFacts(CortrixStore& store,
                                             const std::string& user_id, int limit) {
    std::vector<UserFact> out;
    sqlite3* db = store.db_handle();
    if (!db) return Result<std::vector<UserFact>>(std::move(out));

    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT content_text, metadata_json FROM blocks "
        "WHERE block_type=?1 "
        "AND json_extract(metadata_json,'$.user_id')=?2 "
        "AND json_extract(metadata_json,'$.status')='active' "
        "AND json_extract(metadata_json,'$.memory_type') IS NOT NULL "
        "ORDER BY json_extract(metadata_json,'$.created_at') DESC LIMIT ?3";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return Result<std::vector<UserFact>>(
            Status::Internal("CX_ERR_MEMEXTRACT_STORE: prepare user facts"));
    }
    sqlite3_bind_int(stmt, 1, static_cast<int>(kBlockMemory));
    sqlite3_bind_text(stmt, 2, user_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, limit > 0 ? limit : 20);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char* ct = sqlite3_column_text(stmt, 0);
        const unsigned char* mj = sqlite3_column_text(stmt, 1);
        auto j = nlohmann::json::parse(mj ? reinterpret_cast<const char*>(mj) : "{}",
                                       nullptr, false);
        if (j.is_discarded() || !j.is_object()) continue;
        UserFact f;
        f.content = ct ? reinterpret_cast<const char*>(ct) : "";
        f.block_id = JsonStr(j, "block_id");
        f.memory_type = JsonStr(j, "memory_type");
        f.created_at = JsonStr(j, "created_at");
        if (j.contains("confidence") && j["confidence"].is_number())
            f.confidence = j["confidence"].get<double>();
        out.push_back(std::move(f));
    }
    sqlite3_finalize(stmt);
    return Result<std::vector<UserFact>>(std::move(out));
}

}  // namespace cortrix::memory
