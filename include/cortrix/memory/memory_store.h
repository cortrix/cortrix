#pragma once
#include <cstdint>
#include <atomic>
#include <string>
#include <vector>
#include <mutex>
#include "cortrix/common/status.h"
#include "cortrix/memory/interaction_log.h"
#include "cortrix/memory/memory_session.h"
#include "cortrix/store/cortrix_store.h"

struct sqlite3;

namespace cortrix {

class MemoryStore {
public:
    /// @param store: CortrixStore instance (per-namespace, shared SQLite connection)
    explicit MemoryStore(CortrixStore& store);
    ~MemoryStore();

    /// Initialize with a database path for memory tables (interaction_log, memory_sessions)
    /// @param db_path: SQLite database path (":memory:" for testing)
    Status Init(const std::string& db_path = ":memory:");

    /// Testing seam: force the next `n` public API calls (Session*/
    /// Interaction*) to fail with Status::Internal without touching the db, to
    /// exercise callers' storage-failure branches. `n == 0` disables it. Not
    /// part of the production path (a single relaxed load when disarmed).
    void SetFailNextOps(int n) { fail_next_ops_.store(n, std::memory_order_relaxed); }

    /// Borrowed handle to this namespace's memory.db (interaction_log /
    /// memory_sessions / — after Init — the agent_trace + interaction_sources
    /// tables). The observability handlers (TracesHandler /
    /// InteractionsHandler) read interaction_log, which lives in THIS db (not the
    /// blocks store db); this additive accessor lets the per-request route build
    /// its handlers over the right connection. Not owned; null before Init().
    sqlite3* db_handle() const { return db_; }

    // ---- Session management ----

    /// Create new session (generates UUID v4)
    /// @param session: [in/out] fills session_id + created_at
    Status SessionCreate(MemorySession& session);

    /// Get session metadata
    /// @return Ok / NotFound
    Status SessionGet(const std::string& session_id, MemorySession& session);

    /// List sessions (sorted by updated_at descending). When `user_id` is
    /// non-empty the result is filtered to that owner in SQL — this keeps
    /// pagination correct under per-user isolation (offset/limit apply to
    /// the owner's sessions, not the NS-wide set). Empty `user_id` = no filter.
    Status SessionList(const std::string& namespace_name,
                       int limit, int offset,
                       std::vector<MemorySession>& sessions,
                       const std::string& user_id = "");

    /// Delete session: cascade delete all interaction_log rows
    /// @return Ok / NotFound
    Status SessionDelete(const std::string& session_id);

    // ---- Interaction CRUD ----

    /// Insert one interaction log
    /// @param log: [in/out] fills id (UUID v4) + created_at
    Status InteractionInsert(InteractionLog& log);

    /// Get all interactions for a session (sorted by created_at ascending)
    Status InteractionListBySession(const std::string& session_id,
                                    std::vector<InteractionLog>& interactions);

    /// Get recent N interactions for a session (sorted by created_at descending)
    Status InteractionGetRecent(const std::string& session_id,
                                int limit,
                                std::vector<InteractionLog>& interactions);

    /// Count interactions in a session
    Status InteractionCount(const std::string& session_id, int64_t* count);

    /// Search interactions by content text (SQL LIKE '%query%')
    /// Returns matching interactions sorted by created_at descending
    /// @param namespace_name: namespace to search within
    /// @param query: search text (substring match)
    /// @param session_id: optional session filter (empty = all sessions)
    /// @param user_id: optional user filter (empty = all users)
    /// @param limit: max results (default 10)
    Status InteractionSearch(const std::string& namespace_name,
                             const std::string& query,
                             const std::string& session_id,
                             const std::string& user_id,
                             int limit,
                             std::vector<InteractionLog>& results);

    /// Update session's updated_at + interaction_count
    Status SessionTouch(const std::string& session_id);

    // ---- Memory immunity (opt-out) ----

    /// Read a session's opt-out columns (opt_out_at / opted_out_by).
    /// @param session_id: session to read
    /// @param exists:     [out] true iff the session row exists
    /// @param opt_out_at: [out] ISO 8601 opt-out time, empty when active
    /// @param opted_out_by: [out] actor string, empty when active
    /// @return Ok (check `exists`) / Internal on DB error. Absent session = Ok + exists=false.
    Status SessionGetOptOut(const std::string& session_id,
                            bool* exists,
                            std::string* opt_out_at,
                            std::string* opted_out_by);

    /// Stamp a session as opted-out (sets opt_out_at + opted_out_by). Unconditional
    /// UPDATE of the two columns; does not touch interaction_count / updated_at.
    /// @return Ok / NotFound (no such session) / Internal on DB error.
    Status SessionSetOptOut(const std::string& session_id,
                            const std::string& opt_out_at,
                            const std::string& opted_out_by);

    /// Clear a session's opt-out stamp (opt_out_at + opted_out_by → NULL).
    /// @return Ok / NotFound (no such session) / Internal on DB error.
    Status SessionClearOptOut(const std::string& session_id);

    /// Count sessions in a namespace (for pagination total_count). When
    /// `user_id` is non-empty the count is scoped to that owner so total_count
    /// matches the filtered SessionList page (per-user isolation). Empty = NS-wide.
    Status SessionCount(const std::string& namespace_name, int64_t* count,
                        const std::string& user_id = "");

    /// Atomically insert user + assistant interaction pair and touch the session.
    /// All three operations are performed within a single BEGIN/COMMIT transaction
    /// under one mutex acquisition, ensuring proper serialization.
    /// @param user_log: [in/out] fills id + created_at for user interaction
    /// @param assistant_log: [in/out] fills id + created_at for assistant interaction
    /// @param session_id: session to touch (interaction_count + 1, updated_at refreshed)
    Status InteractionPairInsertAndSessionTouch(InteractionLog& user_log,
                                                InteractionLog& assistant_log,
                                                const std::string& session_id);

private:
    Status CreateInteractionLogTable();
    Status CreateMemorySessionsTable();
    // Create the agent_trace + interaction_sources tables in this memory.db
    // (they live alongside interaction_log, the table the observability handlers read). Runs the
    // frozen AgentTraceSchemaProvider + InteractionSourcesSchemaProvider (idempotent).
    Status CreateAgentTraceObservabilityTables();
    // Idempotently add the opt-out columns + partial index (memory_sessions
    // opt_out_at / opted_out_by; interaction_log remember). SQLite ADD COLUMN is not
    // "if not exists", so guarded on pragma_table_info — same pattern as the SPC
    // schema providers. Pure ADD; the frozen MVP columns are untouched.
    Status MigrateMemoryOptOutColumns();
    static std::string GenerateUUID();
    static std::string NowISO8601();

    // Unlocked variants — must be called with mu_ already held
    Status InsertInteractionLocked(InteractionLog& log);
    Status TouchSessionLocked(const std::string& session_id);

    CortrixStore& store_;
    /// Consume one injected op fault if armed (SetFailNextOps seam).
    bool TryConsumeOpFault();

    sqlite3* db_ = nullptr;
    std::atomic<int> fail_next_ops_{0};  ///< testing seam (SetFailNextOps)
    bool owns_db_ = true;
    mutable std::mutex mu_;
};

}  // namespace cortrix
