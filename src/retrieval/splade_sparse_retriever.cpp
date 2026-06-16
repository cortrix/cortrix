#include <cstdint>
#include "cortrix/retrieval/splade_sparse_retriever.h"

#include <sqlite3.h>

#include <algorithm>
#include <unordered_map>

#include "cortrix/retrieval/f40_schema_provider.h"  // kSparseInvertedIndexDdl (shared SoT)
#include "cortrix/retrieval/sparse_error.h"

namespace cortrix::retrieval {

SpladeSparseRetriever::SpladeSparseRetriever(SpladeConfig config,
                                             std::string db_path)
    : config_(config), db_path_(std::move(db_path)) {}

SpladeSparseRetriever::~SpladeSparseRetriever() { Close(); }

Status SpladeSparseRetriever::Open() {
    std::lock_guard<std::mutex> lk(mu_);
    if (db_) return Status::Ok();  // idempotent

    int rc = sqlite3_open_v2(
        db_path_.c_str(), &db_,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
    if (rc != SQLITE_OK) {
        std::string msg = db_ ? sqlite3_errmsg(db_) : "open failed";
        if (db_) { sqlite3_close(db_); db_ = nullptr; }
        return SparseStatus(SparseErrorCode::kInvertedIndexWriteFailed,
                            "open " + db_path_ + ": " + msg);
    }
    // WAL improves concurrent write throughput (§6.5 concurrent-write row).
    sqlite3_exec(db_, "PRAGMA journal_mode = WAL", nullptr, nullptr, nullptr);

    char* err = nullptr;
    rc = sqlite3_exec(db_, kSparseInvertedIndexDdl, nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        std::string msg = err ? err : "DDL failed";
        sqlite3_free(err);
        sqlite3_close(db_);
        db_ = nullptr;
        return SparseStatus(SparseErrorCode::kInvertedIndexWriteFailed,
                            "create sparse_inverted_index: " + msg);
    }
    return Status::Ok();
}

void SpladeSparseRetriever::Close() {
    std::lock_guard<std::mutex> lk(mu_);
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
    lru_.clear();
    cache_index_.clear();
}

bool SpladeSparseRetriever::IsAvailable() const {
    std::lock_guard<std::mutex> lk(mu_);
    return db_ != nullptr;
}

std::string SpladeSparseRetriever::CacheKey(const NamespaceId& ns_id,
                                            uint32_t term_id) const {
    std::string k = ns_id;
    k.push_back('\0');
    k.append(reinterpret_cast<const char*>(&term_id), sizeof(term_id));
    return k;
}

// --- S5: write / delete (§6.1, §6.2) ---

Status SpladeSparseRetriever::Remove(const NamespaceId& ns_id,
                                     const ChildId& child_id) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!db_) {
        return SparseStatus(SparseErrorCode::kInvertedIndexWriteFailed,
                            "retriever not open");
    }
    // Collect the affected terms first so we can invalidate their cache entries
    // (a delete can change any term's posting list for this child).
    std::vector<uint32_t> affected;
    {
        const char* q =
            "SELECT term_id FROM sparse_inverted_index "
            "WHERE ns_id = ? AND child_id = ?";
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db_, q, -1, &st, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(st, 1, ns_id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st, 2, child_id.c_str(), -1, SQLITE_TRANSIENT);
            while (sqlite3_step(st) == SQLITE_ROW) {
                affected.push_back(static_cast<uint32_t>(sqlite3_column_int64(st, 0)));
            }
        }
        sqlite3_finalize(st);
    }

    const char* del =
        "DELETE FROM sparse_inverted_index WHERE ns_id = ? AND child_id = ?";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, del, -1, &st, nullptr) != SQLITE_OK) {
        return SparseStatus(SparseErrorCode::kInvertedIndexWriteFailed,
                            std::string("prepare delete: ") + sqlite3_errmsg(db_));
    }
    sqlite3_bind_text(st, 1, ns_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, child_id.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        return SparseStatus(SparseErrorCode::kInvertedIndexWriteFailed,
                            std::string("delete child '") + child_id +
                                "': " + sqlite3_errmsg(db_));
    }
    for (uint32_t t : affected) InvalidateTerm(ns_id, t);
    return Status::Ok();
}

Status SpladeSparseRetriever::Add(const NamespaceId& ns_id,
                                  const ChildId& child_id,
                                  const SparseVector& vec) {
    // Empty vector → drop the child (dead chunk, §6.5). Remove takes the lock.
    if (vec.empty()) {
        return Remove(ns_id, child_id);
    }

    std::lock_guard<std::mutex> lk(mu_);
    if (!db_) {
        return SparseStatus(SparseErrorCode::kInvertedIndexWriteFailed,
                            "retriever not open");
    }

    auto fail = [&](const std::string& msg) -> Status {
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        return SparseStatus(SparseErrorCode::kInvertedIndexWriteFailed, msg);
    };

    if (sqlite3_exec(db_, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) != SQLITE_OK) {
        return SparseStatus(SparseErrorCode::kInvertedIndexWriteFailed,
                            std::string("BEGIN: ") + sqlite3_errmsg(db_));
    }

    // Replace semantics: delete existing postings for this child, then insert.
    {
        const char* del =
            "DELETE FROM sparse_inverted_index WHERE ns_id = ? AND child_id = ?";
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db_, del, -1, &st, nullptr) != SQLITE_OK) {
            return fail(std::string("prepare delete: ") + sqlite3_errmsg(db_));
        }
        sqlite3_bind_text(st, 1, ns_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, child_id.c_str(), -1, SQLITE_TRANSIENT);
        int rc = sqlite3_step(st);
        sqlite3_finalize(st);
        if (rc != SQLITE_DONE) {
            return fail(std::string("delete old postings: ") + sqlite3_errmsg(db_));
        }
    }

    {
        const char* ins =
            "INSERT INTO sparse_inverted_index (ns_id, term_id, child_id, weight) "
            "VALUES (?, ?, ?, ?)";
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db_, ins, -1, &st, nullptr) != SQLITE_OK) {
            return fail(std::string("prepare insert: ") + sqlite3_errmsg(db_));
        }
        for (const auto& [term_id, weight] : vec.terms) {
            sqlite3_bind_text(st, 1, ns_id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(st, 2, static_cast<sqlite3_int64>(term_id));
            sqlite3_bind_text(st, 3, child_id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_double(st, 4, static_cast<double>(weight));
            int rc = sqlite3_step(st);
            if (rc != SQLITE_DONE) {
                std::string msg =
                    std::string("insert posting (") + child_id + ", term " +
                    std::to_string(term_id) + "): " + sqlite3_errmsg(db_);
                sqlite3_finalize(st);
                return fail(msg);
            }
            sqlite3_reset(st);
        }
        sqlite3_finalize(st);
    }

    if (sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK) {
        return fail(std::string("COMMIT: ") + sqlite3_errmsg(db_));
    }

    // Invalidate the touched terms' cache (their posting lists changed).
    for (const auto& [term_id, weight] : vec.terms) {
        (void)weight;
        InvalidateTerm(ns_id, term_id);
    }
    return Status::Ok();
}

// --- S6: query (§6.3) + posting-list LRU cache ---

void SpladeSparseRetriever::InvalidateTerm(const NamespaceId& ns_id,
                                           uint32_t term_id) {
    // mu_ held by callers.
    auto it = cache_index_.find(CacheKey(ns_id, term_id));
    if (it != cache_index_.end()) {
        lru_.erase(it->second);
        cache_index_.erase(it);
    }
}

const SpladeSparseRetriever::PostingList&
SpladeSparseRetriever::GetPostingList(const NamespaceId& ns_id, uint32_t term_id) {
    // mu_ held by caller (Search).
    std::string key = CacheKey(ns_id, term_id);
    auto it = cache_index_.find(key);
    if (it != cache_index_.end()) {
        ++cache_hits_;
        lru_.splice(lru_.begin(), lru_, it->second);  // move to MRU
        return it->second->second;
    }
    ++cache_misses_;

    // Miss: load from DB (capped, weight DESC per §6.5 posting_list_cap).
    PostingList pl;
    const char* q =
        "SELECT child_id, weight FROM sparse_inverted_index "
        "WHERE ns_id = ? AND term_id = ? ORDER BY weight DESC LIMIT ?";
    sqlite3_stmt* st = nullptr;
    if (db_ && sqlite3_prepare_v2(db_, q, -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, ns_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 2, static_cast<sqlite3_int64>(term_id));
        sqlite3_bind_int(st, 3, config_.posting_list_cap);
        while (sqlite3_step(st) == SQLITE_ROW) {
            const unsigned char* cid = sqlite3_column_text(st, 0);
            float w = static_cast<float>(sqlite3_column_double(st, 1));
            pl.entries.emplace_back(
                cid ? reinterpret_cast<const char*>(cid) : "", w);
        }
    }
    sqlite3_finalize(st);

    // Insert into LRU front; evict LRU tail if over capacity.
    lru_.emplace_front(key, std::move(pl));
    cache_index_[key] = lru_.begin();
    // (Minor) Guard a misconfigured posting_cache_capacity==0: we return a reference to
    // the entry just inserted at the front, so never evict down to empty -- keep >=1.
    if (lru_.size() > config_.posting_cache_capacity && lru_.size() > 1) {
        auto& victim = lru_.back();
        cache_index_.erase(victim.first);
        lru_.pop_back();
    }
    return lru_.begin()->second;
}

std::vector<SparseHit> SpladeSparseRetriever::Search(const SparseVector& query,
                                                     const NamespaceId& ns_id,
                                                     int top_k) {
    std::lock_guard<std::mutex> lk(mu_);
    last_search_failed_ = false;
    std::vector<SparseHit> out;
    if (!db_) {
        last_search_failed_ = true;  // L2 fallback signal (§7.2)
        return out;
    }
    if (query.empty()) return out;  // empty query → empty result (success)

    int k = (top_k > 0) ? top_k : config_.default_top_k;

    // Score accumulator: child_id → Σ query_weight × chunk_weight (§6.3).
    std::unordered_map<ChildId, float> score_map;
    for (const auto& [term_id, query_weight] : query.terms) {
        const PostingList& posting = GetPostingList(ns_id, term_id);
        for (const auto& [child_id, chunk_weight] : posting.entries) {
            score_map[child_id] += query_weight * chunk_weight;
        }
    }

    out.reserve(score_map.size());
    for (const auto& [child_id, score] : score_map) {
        out.push_back({child_id, score});
    }
    // top-K by score DESC (§6.3). partial_sort when we have more than K.
    if (k > 0 && static_cast<int>(out.size()) > k) {
        std::partial_sort(
            out.begin(), out.begin() + k, out.end(),
            [](const SparseHit& a, const SparseHit& b) { return a.score > b.score; });
        out.resize(k);
    } else {
        std::sort(out.begin(), out.end(),
                  [](const SparseHit& a, const SparseHit& b) {
                      return a.score > b.score;
                  });
    }
    return out;
}

bool SpladeSparseRetriever::last_search_failed() const {
    std::lock_guard<std::mutex> lk(mu_);
    return last_search_failed_;
}

double SpladeSparseRetriever::cache_hit_rate() const {
    std::lock_guard<std::mutex> lk(mu_);
    uint64_t total = cache_hits_ + cache_misses_;
    return total == 0 ? 0.0 : static_cast<double>(cache_hits_) / static_cast<double>(total);
}

}  // namespace cortrix::retrieval
