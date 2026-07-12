#include "cortrix/spc_enricher/enrich_backfill_worker.h"

#include <algorithm>
#include <chrono>
#include <map>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

#include "cortrix/common/block_types.h"
#include "cortrix/id/hash.h"
#include "cortrix/id/ulid.h"
#include "cortrix/logging/logging.h"
#include "cortrix/resource/namespace_facade.h"
#include "cortrix/spc/contextual_store.h"
#include "cortrix/spc/enricher_chain.h"
#include "cortrix/spc/hype_block.h"
#include "cortrix/spc/onnx_embedder.h"
#include "cortrix/spc/parser.h"  // DocumentMetadata
#include "cortrix/spc_enricher/enrich_state_store.h"
#include "cortrix/spc_enricher/enricher_store.h"
#include "cortrix/store/write_coordinator.h"

namespace cortrix::spc {

namespace {

int64_t NowUnix() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::vector<std::string> SplitCsv(const std::string& csv) {
    std::vector<std::string> out;
    std::stringstream ss(csv);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        if (!tok.empty()) out.push_back(tok);
    }
    return out;
}

std::string JoinCsv(const std::vector<std::string>& toks) {
    std::string out;
    for (const auto& t : toks) {
        if (!out.empty()) out += ",";
        out += t;
    }
    return out;
}

// Fold the repaired F03 summary into blocks.metadata_json (the ingest path bakes
// it in at assembly; backfill patches the queryable column in place).
void UpdateBlockMetadataSummary(sqlite3* db, uint64_t block_id,
                                const std::string& summary) {
    if (!db || summary.empty()) return;
    sqlite3_stmt* sel = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT metadata_json FROM blocks WHERE block_id=?1",
                           -1, &sel, nullptr) != SQLITE_OK) {
        return;
    }
    sqlite3_bind_int64(sel, 1, static_cast<sqlite3_int64>(block_id));
    std::string meta;
    if (sqlite3_step(sel) == SQLITE_ROW) {
        const unsigned char* m = sqlite3_column_text(sel, 0);
        meta = m ? reinterpret_cast<const char*>(m) : "";
    }
    sqlite3_finalize(sel);
    nlohmann::json j = meta.empty()
                           ? nlohmann::json::object()
                           : nlohmann::json::parse(meta, nullptr, false);
    if (j.is_discarded() || !j.is_object()) j = nlohmann::json::object();
    j["summary"] = summary;
    const std::string updated = j.dump();
    sqlite3_stmt* upd = nullptr;
    if (sqlite3_prepare_v2(db, "UPDATE blocks SET metadata_json=?1 WHERE block_id=?2",
                           -1, &upd, nullptr) != SQLITE_OK) {
        return;
    }
    sqlite3_bind_text(upd, 1, updated.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(upd, 2, static_cast<sqlite3_int64>(block_id));
    sqlite3_step(upd);
    sqlite3_finalize(upd);
}

bool BlockHasEnrichedScore(sqlite3* db, uint64_t block_id) {
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(
            db, "SELECT 1 FROM blocks WHERE block_id=?1 AND enriched_score IS NOT NULL",
            -1, &st, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(block_id));
    const bool has = sqlite3_step(st) == SQLITE_ROW;
    sqlite3_finalize(st);
    return has;
}

// child_ids that already have hype blocks (block_type=16 metadata source_child_id)
// — the crash-window / re-run duplicate guard for F38 writes.
std::unordered_set<std::string> HypeSourceChildIds(sqlite3* db) {
    std::unordered_set<std::string> out;
    sqlite3_stmt* st = nullptr;
    const char* sql =
        "SELECT DISTINCT json_extract(metadata_json, '$.source_child_id')"
        " FROM blocks WHERE block_type=16 AND metadata_json IS NOT NULL";
    if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) return out;
    while (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char* c = sqlite3_column_text(st, 0);
        if (c) out.insert(reinterpret_cast<const char*>(c));
    }
    sqlite3_finalize(st);
    return out;
}

}  // namespace

int64_t EnrichBackfillWorker::NextRetryDelaySec(int attempts) {
    static constexpr int64_t kSchedule[] = {60, 300, 900, 3600, 21600};
    constexpr int n = static_cast<int>(sizeof(kSchedule) / sizeof(kSchedule[0]));
    if (attempts < 0) attempts = 0;
    return kSchedule[std::min(attempts, n - 1)];
}

EnrichBackfillWorker::EnrichBackfillWorker(resource::INamespacePool& pool,
                                           EnricherChain* chain,
                                           OnnxEmbedder& embedder,
                                           async::TaskManager* task_mgr)
    : pool_(pool), chain_(chain), embedder_(embedder), finalizer_(task_mgr) {}

Status EnrichBackfillWorker::ProcessTask(const async::TaskInfo& task) {
    const auto t_start = std::chrono::steady_clock::now();

    if (chain_ == nullptr || !chain_->AnyAvailable()) {
        // Transient config state (LLM roles removed / not yet up): rows stay
        // pending; the sweeper re-enqueues once a chain member is back.
        return finalizer_.Fail(task, "CX_ERR_ENRICH_BACKFILL_NO_CHAIN",
                               "enricher chain unavailable",
                               {{"doc_id", task.doc_id}}, t_start);
    }

    resource::NamespaceFacade facade(pool_, task.namespace_id);
    Status acq = facade.Acquire();
    if (!acq.ok()) {
        return finalizer_.Fail(task, "CX_ERR_ENRICH_BACKFILL_NS_ACQUIRE",
                               acq.message(), {{"doc_id", task.doc_id}}, t_start);
    }
    sqlite3* db = facade.store().db_handle();
    if (!db) {
        return finalizer_.Fail(task, "CX_ERR_ENRICH_BACKFILL_STORE",
                               "store db unavailable", {{"doc_id", task.doc_id}},
                               t_start);
    }

    auto pending_r =
        ListEnrichStateForDoc(db, task.doc_id, kEnrichStatusPendingRetry);
    if (!pending_r.ok()) {
        return finalizer_.Fail(task, "CX_ERR_ENRICH_BACKFILL_STORE",
                               pending_r.status().message(),
                               {{"doc_id", task.doc_id}}, t_start);
    }
    std::vector<EnrichStateRow> pending = std::move(pending_r.value());
    if (pending.empty()) {
        return finalizer_.Complete(task, task.doc_id, t_start);  // idempotent no-op
    }

    // ---- Rebuild the doc's chunk contexts from the persisted rows -------------
    std::vector<CortrixBlock> blocks;
    if (facade.store().block_get_by_doc(task.doc_id, blocks) != 0) {
        return finalizer_.Fail(task, "CX_ERR_ENRICH_BACKFILL_STORE",
                               "block_get_by_doc failed",
                               {{"doc_id", task.doc_id}}, t_start);
    }
    std::vector<const CortrixBlock*> children;  // full child order (neighbor context)
    for (const auto& b : blocks) {
        if (!b.child_id.empty()) children.push_back(&b);
    }
    std::sort(children.begin(), children.end(),
              [](const CortrixBlock* a, const CortrixBlock* b) {
                  return a->chunk_index < b->chunk_index;
              });
    std::unordered_map<std::string, size_t> child_pos;
    for (size_t i = 0; i < children.size(); ++i) {
        child_pos[children[i]->child_id] = i;
    }

    CortrixDoc doc;
    DocumentMetadata dm;  // best-effort rebuild (parse-time metadata is not persisted)
    if (facade.store().doc_get(task.doc_id, doc) == 0) {
        dm.filename = doc.source_path;
        dm.doc_title = doc.title;
        dm.mime_type = doc.mime_type;
        dm.file_size_bytes = doc.file_size;
        dm.doc_language = doc.language;
    }

    std::unordered_map<std::string, std::string> parent_text_by_id;
    auto parent_text_of = [&](const std::string& parent_id) -> const std::string& {
        static const std::string kEmpty;
        if (parent_id.empty()) return kEmpty;
        auto it = parent_text_by_id.find(parent_id);
        if (it != parent_text_by_id.end()) return it->second;
        CortrixParent p;
        std::string text;
        if (facade.store().parent_get(parent_id, p) == 0) text = p.parent_text;
        return parent_text_by_id.emplace(parent_id, std::move(text)).first->second;
    };

    // ---- Group pending rows by owed members; re-run only those members --------
    std::map<std::string, std::vector<const EnrichStateRow*>> groups;
    for (const auto& row : pending) {
        if (child_pos.find(row.child_id) == child_pos.end()) continue;  // stale row
        groups[row.failed_members.empty() ? "f03,f35,f38" : row.failed_members]
            .push_back(&row);
    }

    struct Repair {
        const EnrichStateRow* row = nullptr;
        const CortrixBlock* block = nullptr;
        EnrichResult merged;
        std::vector<HypeQuestion> hype;
        std::vector<std::string> owed_before;
        std::vector<std::string> still_owed;
        std::string last_error;
    };
    std::vector<Repair> repairs;

    for (auto& [members_csv, rows] : groups) {
        std::unordered_set<std::string> filter;
        for (const auto& t : SplitCsv(members_csv)) filter.insert(t);

        std::vector<ChunkContext> ctxs;
        std::vector<std::string> parent_texts, src_child_ids, src_parent_ids;
        std::vector<const EnrichStateRow*> group_rows;
        for (const EnrichStateRow* row : rows) {
            const size_t pos = child_pos[row->child_id];
            const CortrixBlock* b = children[pos];
            ChunkContext c;
            c.chunk_text = b->content_text;
            c.chunk_index = b->chunk_index;
            c.doc_metadata = &dm;
            c.prev_chunk_text = pos > 0 ? children[pos - 1]->content_text : "";
            c.next_chunk_text =
                pos + 1 < children.size() ? children[pos + 1]->content_text : "";
            ctxs.push_back(std::move(c));
            parent_texts.push_back(parent_text_of(b->parent_id));
            src_child_ids.push_back(b->child_id);
            src_parent_ids.push_back(b->parent_id);
            group_rows.push_back(row);
        }

        std::vector<ChunkChainResult> res = chain_->EnrichChunks(
            ctxs, parent_texts, src_child_ids, src_parent_ids, &filter);

        for (size_t i = 0; i < res.size() && i < group_rows.size(); ++i) {
            Repair rep;
            rep.row = group_rows[i];
            rep.block = children[child_pos[rep.row->child_id]];
            rep.owed_before = SplitCsv(members_csv);
            // Member-aware outcome (same rules as the ingest write phase).
            bool f03_failed = false, f35_failed = false, f38_failed = false;
            for (const auto& st : res[i].steps) {
                if (st.skipped) continue;
                if (st.status == 0) {
                    // F35 fail-soft: status==0 with a populated error_code (see
                    // enricher_chain). Harvest it — this was the writer that left
                    // 4,139 f35 debt rows with a blank last_error on the 2026-07-11
                    // live 5k ingest (D12).
                    if (rep.last_error.empty() && !st.error_code.empty()) {
                        rep.last_error = st.error_code;
                    }
                    continue;
                }
                const std::string tok = ChainMemberToken(st.name);
                if (tok == "f38") f38_failed = true;
                else if (tok == "f35") f35_failed = true;
                else f03_failed = true;
                if (rep.last_error.empty() && !st.error_code.empty()) {
                    rep.last_error = st.error_code;
                }
            }
            if (res[i].merged.contextualized_status == 2) f35_failed = true;
            for (const auto& tok : rep.owed_before) {
                if ((tok == "f03" && f03_failed) || (tok == "f35" && f35_failed) ||
                    (tok == "f38" && f38_failed)) {
                    rep.still_owed.push_back(tok);
                }
            }
            rep.merged = std::move(res[i].merged);
            rep.hype = std::move(res[i].hype_questions);
            repairs.push_back(std::move(rep));
        }
    }

    // ---- Assemble the write set (hype blocks + vector points) -----------------
    const std::unordered_set<std::string> existing_hype = HypeSourceChildIds(db);
    auto owes = [](const Repair& r, const char* tok) {
        return std::find(r.owed_before.begin(), r.owed_before.end(), tok) !=
                   r.owed_before.end() &&
               std::find(r.still_owed.begin(), r.still_owed.end(), tok) ==
                   r.still_owed.end();
    };

    std::vector<CortrixBlock> new_blocks;
    std::vector<std::pair<const float*, uint64_t>> vec_points;
    std::vector<ContextualVecLabelRow> label_rows;
    std::vector<BlockId> new_block_ids;

    for (auto& rep : repairs) {
        // F35: contextual dual-vector point + label (skip when already indexed —
        // the crash-window guard; the columns update below is idempotent anyway).
        if (owes(rep, "f35") && rep.merged.contextualized_embedding.has_value() &&
            !rep.merged.contextualized_embedding->empty()) {
            const uint64_t label = DeriveContextualVecLabel(rep.block->child_id);
            if (!GetContextualVecLabel(db, label).ok()) {
                vec_points.push_back(
                    {rep.merged.contextualized_embedding->data(), label});
                label_rows.push_back({label, rep.block->block_id, rep.block->child_id});
            }
        }
        // F38: hype blocks (skip children that already have them).
        if (owes(rep, "f38") && !rep.hype.empty() &&
            existing_hype.find(rep.block->child_id) == existing_hype.end()) {
            std::vector<std::string> q_texts;
            for (const auto& q : rep.hype) q_texts.push_back(q.question_text);
            std::vector<EmbeddingResult> q_embs;
            Status qes = embedder_.EmbedBatch(q_texts, &q_embs);
            if (!qes.ok()) {
                // Hype embed failure keeps f38 owed for the next retry round.
                rep.still_owed.push_back("f38");
                if (rep.last_error.empty()) {
                    rep.last_error = "F38 hype embedding failed: " + qes.message();
                }
                continue;
            }
            for (size_t i = 0; i < rep.hype.size() && i < q_embs.size(); ++i) {
                HypeQuestion& q = rep.hype[i];
                q.embedding = q_embs[i].vector;
                const std::string hype_ulid = id::GenerateUlid();
                CortrixBlock hb;
                hb.doc_id = task.doc_id;
                hb.chunk_index = 0;
                hb.block_type = static_cast<int>(kBlockHypeQuestion);
                hb.processing_level = rep.block->processing_level;
                hb.block_id = id::HashChildIdToBlockId(hype_ulid);
                hb.content_text = q.question_text;
                hb.metadata_json = BuildHypeQuestionMetadataJson(
                    q, /*prompt_version=*/"v1", /*llm_model=*/"gpt-4o-mini",
                    /*cost_usd=*/0.0, /*generated_at=*/"");
                hb.data = BuildHypeQuestionBlock(q, "v1", "gpt-4o-mini", 0.0, "");
                // The point borrows q.embedding storage: q lives in rep.hype
                // (repairs is fully built — no reallocation before AddPoints).
                if (!q.embedding.empty()) {
                    vec_points.push_back({q.embedding.data(), hb.block_id});
                }
                new_block_ids.push_back(hb.block_id);
                new_blocks.push_back(std::move(hb));
            }
        }
    }

    // ---- Persist (one F25 txn when blocks/vectors are added) ------------------
    const bool needs_txn = !new_blocks.empty() || !vec_points.empty();
    store::TxnHandle txn;
    if (needs_txn) {
        Status bw = facade.write_coordinator().BeginWrite(
            task.doc_id, new_block_ids, /*writes_blob=*/false, &txn);
        if (!bw.ok()) {
            return finalizer_.Fail(task, "CX_ERR_ENRICH_BACKFILL_WRITE",
                                   bw.message(), {{"doc_id", task.doc_id}}, t_start);
        }
        if (!vec_points.empty()) {
            Status av = facade.vec_index().AddPoints(vec_points);
            if (!av.ok()) {
                facade.write_coordinator().Rollback(txn);
                return finalizer_.Fail(task, "CX_ERR_ENRICH_BACKFILL_WRITE",
                                       av.message(), {{"doc_id", task.doc_id}},
                                       t_start);
            }
        }
        for (auto& hb : new_blocks) {
            if (facade.store().block_insert(hb) != 0) {
                facade.write_coordinator().Rollback(txn);
                return finalizer_.Fail(task, "CX_ERR_ENRICH_BACKFILL_WRITE",
                                       "hype block_insert failed",
                                       {{"doc_id", task.doc_id}}, t_start);
            }
        }
    }

    // Column/row-level artifacts (idempotent UPDATEs). A persist FAILURE must not
    // count as repaired (P3): keeping the member in still_owed re-arms the retry so
    // the next round re-runs the idempotent write, instead of flipping the row to ok
    // with the columns permanently missing.
    for (auto& rep : repairs) {
        if (owes(rep, "f03") && rep.merged.ok() &&
        !BlockHasEnrichedScore(db, rep.block->block_id)) {
            Status we = WriteEnrichment(db, rep.block->block_id, rep.merged);
            if (!we.ok()) {
                CORTRIX_LOG_WARN("spc", "backfill F03 persist failed, keep owed block_id={}: {}",
                                 rep.block->block_id, we.message());
                rep.still_owed.push_back("f03");
                // QA 2026-07-12 F-5: without this, the unconditional flip below
                // rewrites the row's last_error from an empty rep.last_error and
                // blanks the diagnosis ingest already recorded (the exact blank-
                // last_error shape D12 closed on the ingest side).
                if (rep.last_error.empty()) {
                    rep.last_error =
                        "CX_ERR_SPC_PERSIST_FAILED[f03]: " + we.message();
                }
            } else {
                UpdateBlockMetadataSummary(db, rep.block->block_id, rep.merged.summary);
            }
        }
        if (owes(rep, "f35")) {
            Status wc = WriteContextualized(db, rep.block->block_id, rep.merged);
            if (!wc.ok()) {
                CORTRIX_LOG_WARN("spc", "backfill F35 persist failed, keep owed block_id={}: {}",
                                 rep.block->block_id, wc.message());
                rep.still_owed.push_back("f35");
                if (rep.last_error.empty()) {
                    rep.last_error =
                        "CX_ERR_SPC_PERSIST_FAILED[f35]: " + wc.message();
                }
            }
        }
    }
    for (const auto& lr : label_rows) {
        Status wl = WriteContextualVecLabel(db, lr);
        if (!wl.ok()) {
            CORTRIX_LOG_WARN("spc", "backfill ctx label persist skipped child={}: {}",
                             lr.child_id, wl.message());
        }
    }

    if (needs_txn) {
        Status cm = facade.write_coordinator().Commit(txn);
        if (!cm.ok()) {
            return finalizer_.Fail(task, "CX_ERR_ENRICH_BACKFILL_WRITE", cm.message(),
                                   {{"doc_id", task.doc_id}}, t_start);
        }
    }

    // ---- Flip enrich_state ----------------------------------------------------
    const int64_t now = NowUnix();
    int repaired = 0, still_pending = 0, gave_up = 0;
    for (const auto& rep : repairs) {
        EnrichStateRow row = *rep.row;
        row.attempts += 1;
        row.updated_at = now;
        if (rep.still_owed.empty()) {
            row.status = kEnrichStatusOk;
            row.failed_members.clear();
            row.last_error.clear();
            row.next_retry_at = 0;
            ++repaired;
        } else {
            row.failed_members = JoinCsv(rep.still_owed);
            row.last_error = rep.last_error.substr(0, 200);
            if (row.attempts >= kMaxAttempts) {
                row.status = kEnrichStatusFailedPermanent;
                row.next_retry_at = 0;
                ++gave_up;
                CORTRIX_LOG_WARN("spc",
                    "enrich backfill gave up child={} doc={} owed={} after {} attempts",
                    row.child_id, row.doc_id, row.failed_members, row.attempts);
            } else {
                row.status = kEnrichStatusPendingRetry;
                row.next_retry_at = now + NextRetryDelaySec(row.attempts);
                ++still_pending;
            }
        }
        Status us = UpsertEnrichState(db, row);
        if (!us.ok()) {
            CORTRIX_LOG_WARN("spc", "enrich_state flip failed child={}: {}",
                             row.child_id, us.message());
        }
    }

    CORTRIX_LOG_INFO("spc",
        "enrich backfill doc={} ns={} repaired={} still_pending={} gave_up={}",
        task.doc_id, task.namespace_id, repaired, still_pending, gave_up);
    return finalizer_.Complete(task, task.doc_id, t_start);
}

}  // namespace cortrix::spc
