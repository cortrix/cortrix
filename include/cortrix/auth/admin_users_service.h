#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "cortrix/auth/password_utils.h"
#include "cortrix/auth/user_info.h"
#include "cortrix/common/result.h"
#include "cortrix/common/status.h"

typedef struct sqlite3 sqlite3;

namespace cortrix::auth {

/// One admin/users record (P08 §2.13-bis UserRecord). The richer profile the
/// admin UsersPage shows: the platform.db `users` row projected to the
/// Agent-friendly response shape. `role` is 'admin' | 'user'; `status` is
/// 'active' | 'disabled' (the API never surfaces the internal 'locked' state —
/// a locked account is reported as 'active', the lockout is a transient login
/// concern, not an admin-managed status).
struct AdminUserRecord {
    std::string id;             ///< "usr_" + 8 hex
    std::string email;
    std::string display_name;
    std::string role;           ///< admin | user
    std::string status;         ///< active | disabled
    bool email_verified = false;
    int64_t created_at = 0;     ///< Unix seconds
};

/// Filter for the list endpoint (P08 §2.13-bis GET /admin/users query params).
struct AdminUserListFilter {
    std::optional<std::string> q;       ///< substring match on email OR display_name
    std::optional<std::string> status;  ///< active | disabled
    std::optional<std::string> role;    ///< admin | user
    int page = 1;                       ///< 1-based; <1 clamped to 1
    int limit = 20;                     ///< default 20, cap 200
};

/// Paginated list result (P08 §2.13-bis `{ users, total, page, limit }`).
struct AdminUserListResult {
    std::vector<AdminUserRecord> users;
    int64_t total = 0;
    int page = 1;
    int limit = 20;
};

/// Admin user-management service over platform.db `users` (P08 §2.13-bis, the 5
/// admin/users endpoints). Standalone CRUD-ish logic that the route layer
/// (auth_routes.cpp RegisterAdminUsersRoutes) wraps with WithAuth(kPermAdmin) +
/// operation_log writes. Errors are carried as AuthStatus(...) tokens
/// (CX_ERR_USER_* / CX_ERR_INVALID_REQUEST) re-inflated to the GEN-Agent body at
/// the boundary, exactly like AuthService.
///
/// Borrows an open platform.db handle + an IPasswordHasher (for invite-mode
/// create). Does NOT own either. SQLite serializes the writes; no extra locking.
class AdminUsersService {
public:
    AdminUsersService(sqlite3* platform_db, IPasswordHasher* hasher)
        : db_(platform_db), hasher_(hasher) {}

    /// List users with optional q/status/role filters + pagination (§2.13-bis).
    Result<AdminUserListResult> List(const AdminUserListFilter& filter);

    /// Create a user in admin invite mode — no email-verification flow
    /// (email_verified defaults true, §2.13-bis "no email verify"). Validates
    /// email format / password complexity / role enum. Errors:
    /// CX_ERR_USER_VALIDATION (bad email/role/empty) / CX_ERR_INVALID_REQUEST
    /// (weak password) / CX_ERR_USER_EMAIL_EXISTS (duplicate email).
    Result<AdminUserRecord> Create(const std::string& email,
                                   const std::string& password,
                                   const std::string& role,
                                   const std::string& display_name);

    /// Update display_name / role / email_verified (§2.13-bis PATCH). Each field
    /// is optional; absent fields are untouched. Errors: CX_ERR_USER_NOT_FOUND /
    /// CX_ERR_USER_VALIDATION (bad role / empty display_name).
    Result<AdminUserRecord> Update(const std::string& id,
                                   const std::optional<std::string>& display_name,
                                   const std::optional<std::string>& role,
                                   const std::optional<bool>& email_verified);

    /// Disable a user: status active → disabled (soft, no delete, §2.13-bis).
    /// Idempotent (already-disabled returns the row). Error: CX_ERR_USER_NOT_FOUND.
    Result<AdminUserRecord> Disable(const std::string& id);

    /// Enable a user: status disabled → active (§2.13-bis). Idempotent. Error:
    /// CX_ERR_USER_NOT_FOUND.
    Result<AdminUserRecord> Enable(const std::string& id);

    /// Fetch one record by id (helper, also used internally). Error:
    /// CX_ERR_USER_NOT_FOUND.
    Result<AdminUserRecord> Get(const std::string& id);

private:
    /// Set status ('active' | 'disabled') for `id` and return the updated row.
    Result<AdminUserRecord> SetStatus(const std::string& id, const std::string& status);

    sqlite3* db_;
    IPasswordHasher* hasher_;
};

}  // namespace cortrix::auth
