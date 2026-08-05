#pragma once
#include <string>

#include "cortrix/common/result.h"
#include "cortrix/common/status.h"
#include "cortrix/resource/namespace_pool_config.h"  // SqlitePragmas

struct sqlite3;  // opaque, same fwd-decl style as cortrix_store_sqlite.h

namespace cortrix::resource {

/// RAII owner of a per-Unit store.db connection (UnitResourceBundle.store_db).
///
/// The pool opens one of these per Unit during load and applies the tuned PRAGMAs
/// (§7). The bundle owns it via unique_ptr, so closing the SQLite handle is tied
/// to bundle lifetime — dropping a NamespaceResourceBundle releases its index,
/// WriteCoordinator and every store.db connection together (UT2).
///
/// MVP store code (CortrixStoreSqlite) keeps its own sqlite3* and is not reused
/// here: the pool owns the runtime *resource object* (the open connection), whereas
/// CortrixStoreSqlite owns the doc/block *schema + queries*. Wiring the pool's
/// connection through to the MVP store layer is integration → D3.5.
class SqliteConn {
public:
    SqliteConn() = default;
    ~SqliteConn();

    SqliteConn(const SqliteConn&) = delete;
    SqliteConn& operator=(const SqliteConn&) = delete;
    SqliteConn(SqliteConn&& other) noexcept;
    SqliteConn& operator=(SqliteConn&& other) noexcept;

    /// Open `db_path` (created if absent). Returns CX_ERR_NS_LOAD_FAILED-coded
    /// Status (kUnavailable) on failure; the caller maps it to PoolError.
    Status Open(const std::string& db_path);

    /// Apply the 6 tuned PRAGMAs. No-op (returns Ok) when not open. PRAGMA
    /// failures are logged-and-tolerated rather than fatal: a missing PRAGMA
    /// degrades performance, it does not corrupt data (§7.1).
    Status ApplyPragmas(const SqlitePragmas& pragmas);

    /// Borrowed handle, nullptr before Open / after a moved-from state. The pool
    /// hands this to the store layer at D3.5; never owned by the caller.
    sqlite3* handle() const { return db_; }
    bool is_open() const { return db_ != nullptr; }

    const std::string& path() const { return path_; }

    /// Read back a PRAGMA's current value (e.g. "journal_mode") for verification
    /// (UT17). Returns the value Result or a kInternal Status on query failure.
    Result<std::string> ReadPragma(const std::string& pragma_name) const;

private:
    void Close();

    sqlite3* db_ = nullptr;
    std::string path_;
};

}  // namespace cortrix::resource
