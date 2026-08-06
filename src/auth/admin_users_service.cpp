#include "cortrix/auth/admin_users_service.h"

#include <sqlite3.h>

#include <chrono>
#include <string>

#include <nlohmann/json.hpp>

#include "cortrix/auth/auth_error.h"

namespace cortrix::auth {

namespace {

constexpr int kDisplayNameMax = 64;  // display_name 1..64
constexpr int kListLimitCap = 200;   // pagination cap

int64_t NowSec() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

bool ValidRole(const std::string& role) {
    return role == "admin" || role == "user";
}

// The API surfaces only 'active' | 'disabled': the internal
// transient 'locked' state is reported as 'active' (it is a login concern, not
// an admin-managed status).
std::string ApiStatus(const std::string& db_status) {
    return db_status == "disabled" ? "disabled" : "active";
}

const char* TextCol(sqlite3_stmt* stmt, int col) {
    const char* t = reinterpret_cast<const char*>(sqlite3_column_text(stmt, col));
    return t ? t : "";
}

// Project the canonical SELECT column order into an AdminUserRecord.
//   0:id 1:email 2:display_name 3:role 4:status 5:email_verified 6:created_at
AdminUserRecord RowToRecord(sqlite3_stmt* stmt) {
    AdminUserRecord r;
    r.id = TextCol(stmt, 0);
    r.email = TextCol(stmt, 1);
    r.display_name = TextCol(stmt, 2);
    r.role = TextCol(stmt, 3);
    r.status = ApiStatus(TextCol(stmt, 4));
    r.email_verified = sqlite3_column_int(stmt, 5) != 0;
    r.created_at = sqlite3_column_int64(stmt, 6);
    return r;
}

constexpr const char* kSelectCols =
    "id, email, display_name, role, status, email_verified, created_at";

}  // namespace

Result<AdminUserRecord> AdminUsersService::Get(const std::string& id) {
    if (db_ == nullptr) {
        return AuthStatus(AuthErrorCode::kInternalError, "AdminUsersService not initialized");
    }
    sqlite3_stmt* stmt = nullptr;
    const std::string sql =
        std::string("SELECT ") + kSelectCols + " FROM users WHERE id = ?";
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        return AuthStatus(AuthErrorCode::kServiceUnavailable, "user lookup prepare failed");
    }
    sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    bool found = false;
    AdminUserRecord rec;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        found = true;
        rec = RowToRecord(stmt);
    }
    sqlite3_finalize(stmt);
    if (!found) return AuthStatus(AuthErrorCode::kUserNotFound, id);
    return rec;
}

Result<AdminUserListResult> AdminUsersService::List(const AdminUserListFilter& filter) {
    if (db_ == nullptr) {
        return AuthStatus(AuthErrorCode::kInternalError, "AdminUsersService not initialized");
    }

    // Clamp pagination (page 1-based, limit default 20 cap 200).
    int page = filter.page < 1 ? 1 : filter.page;
    int limit = filter.limit;
    if (limit < 1) limit = 20;
    if (limit > kListLimitCap) limit = kListLimitCap;
    const int offset = (page - 1) * limit;

    // Build the shared WHERE from the optional filters. Bind values in order.
    std::string where;
    std::vector<std::string> binds;
    auto add = [&](const std::string& clause, const std::string& value) {
        where += where.empty() ? " WHERE " : " AND ";
        where += clause;
        binds.push_back(value);
    };
    if (filter.q && !filter.q->empty()) {
        where += where.empty() ? " WHERE " : " AND ";
        where += "(email LIKE ? OR display_name LIKE ?)";
        const std::string like = "%" + *filter.q + "%";
        binds.push_back(like);
        binds.push_back(like);
    }
    if (filter.status && !filter.status->empty()) {
        // 'active' must also include the internal 'locked' rows (ApiStatus folds
        // locked → active), so filter on NOT 'disabled' instead of = 'active'.
        if (*filter.status == "disabled") {
            add("status = ?", "disabled");
        } else {
            where += where.empty() ? " WHERE " : " AND ";
            where += "status != ?";
            binds.push_back("disabled");
        }
    }
    if (filter.role && !filter.role->empty()) {
        add("role = ?", *filter.role);
    }

    // 1) total count over the same predicate.
    AdminUserListResult out;
    out.page = page;
    out.limit = limit;
    {
        const std::string csql = "SELECT COUNT(*) FROM users" + where;
        sqlite3_stmt* cstmt = nullptr;
        if (sqlite3_prepare_v2(db_, csql.c_str(), -1, &cstmt, nullptr) != SQLITE_OK) {
            return AuthStatus(AuthErrorCode::kServiceUnavailable, "users count prepare failed");
        }
        for (size_t i = 0; i < binds.size(); ++i) {
            sqlite3_bind_text(cstmt, static_cast<int>(i + 1), binds[i].c_str(), -1,
                              SQLITE_TRANSIENT);
        }
        if (sqlite3_step(cstmt) == SQLITE_ROW) {
            out.total = sqlite3_column_int64(cstmt, 0);
        }
        sqlite3_finalize(cstmt);
    }

    // 2) the page (newest first, stable secondary by id).
    const std::string lsql = std::string("SELECT ") + kSelectCols + " FROM users" + where +
                             " ORDER BY created_at DESC, id DESC LIMIT ? OFFSET ?";
    sqlite3_stmt* lstmt = nullptr;
    if (sqlite3_prepare_v2(db_, lsql.c_str(), -1, &lstmt, nullptr) != SQLITE_OK) {
        return AuthStatus(AuthErrorCode::kServiceUnavailable, "users list prepare failed");
    }
    int idx = 1;
    for (const auto& b : binds) {
        sqlite3_bind_text(lstmt, idx++, b.c_str(), -1, SQLITE_TRANSIENT);
    }
    sqlite3_bind_int(lstmt, idx++, limit);
    sqlite3_bind_int(lstmt, idx++, offset);
    while (sqlite3_step(lstmt) == SQLITE_ROW) {
        out.users.push_back(RowToRecord(lstmt));
    }
    sqlite3_finalize(lstmt);
    return out;
}

Result<AdminUserRecord> AdminUsersService::Create(const std::string& email,
                                                  const std::string& password,
                                                  const std::string& role,
                                                  const std::string& display_name) {
    if (db_ == nullptr || hasher_ == nullptr) {
        return AuthStatus(AuthErrorCode::kInternalError, "AdminUsersService not initialized");
    }

    // 1) Validation. Format / role / display_name → 422 CX_ERR_USER_VALIDATION;
    //    weak password → 422 (mapped here too; the frontend treats both as 422).
    nlohmann::json invalid = nlohmann::json::array();
    if (!ValidateEmail(email)) invalid.push_back("email");
    if (!ValidRole(role)) invalid.push_back("role");
    // display_name is optional on create; if provided it must be 1..64.
    if (!display_name.empty() && display_name.size() > kDisplayNameMax)
        invalid.push_back("display_name");
    if (!ValidatePassword(password, /*min_length=*/8)) invalid.push_back("password");
    if (!invalid.empty()) {
        return AuthStatus(AuthErrorCode::kUserValidation,
                          "fields_invalid=" + invalid.dump());
    }

    // 2) Hash + insert (the email UNIQUE constraint is the race guarantee).
    Result<std::string> hash = hasher_->Hash(password);
    if (!hash.ok()) return hash.status();

    const std::string user_id = GenerateUserId();
    const int64_t now = NowSec();
    // Admin invite mode: no email-verification flow → email_verified=1.
    const std::string dn = display_name.empty() ? email : display_name;

    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "INSERT INTO users(id, email, password_hash, display_name, email_verified, "
        "role, status, login_attempts, created_at, updated_at) "
        "VALUES(?, ?, ?, ?, 1, ?, 'active', 0, ?, ?)";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return AuthStatus(AuthErrorCode::kServiceUnavailable, "user insert prepare failed");
    }
    sqlite3_bind_text(stmt, 1, user_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, email.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, hash.value().c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, dn.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, role.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 6, now);
    sqlite3_bind_int64(stmt, 7, now);
    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        if (rc == SQLITE_CONSTRAINT) {
            return AuthStatus(AuthErrorCode::kUserEmailExists, email);
        }
        return AuthStatus(AuthErrorCode::kServiceUnavailable, "user insert failed");
    }

    AdminUserRecord rec;
    rec.id = user_id;
    rec.email = email;
    rec.display_name = dn;
    rec.role = role;
    rec.status = "active";
    rec.email_verified = true;
    rec.created_at = now;
    return rec;
}

Result<AdminUserRecord> AdminUsersService::Update(
    const std::string& id, const std::optional<std::string>& display_name,
    const std::optional<std::string>& role, const std::optional<bool>& email_verified) {
    if (db_ == nullptr) {
        return AuthStatus(AuthErrorCode::kInternalError, "AdminUsersService not initialized");
    }

    // Existence first → 404 if absent.
    auto existing = Get(id);
    if (!existing.ok()) return existing.status();

    // Validate the present fields.
    nlohmann::json invalid = nlohmann::json::array();
    if (role && !ValidRole(*role)) invalid.push_back("role");
    if (display_name && (display_name->empty() || display_name->size() > kDisplayNameMax))
        invalid.push_back("display_name");
    if (!invalid.empty()) {
        return AuthStatus(AuthErrorCode::kUserValidation,
                          "fields_invalid=" + invalid.dump());
    }

    // Nothing to change → return the row as-is (idempotent no-op PATCH).
    if (!display_name && !role && !email_verified) {
        return existing.value();
    }

    std::string set;
    std::vector<std::string> text_binds;  // display_name / role
    std::vector<int> int_binds;            // email_verified (0/1)
    auto addText = [&](const std::string& col, const std::string& v) {
        set += set.empty() ? "SET " : ", ";
        set += col + " = ?";
        text_binds.push_back(v);
    };
    if (display_name) addText("display_name", *display_name);
    if (role) addText("role", *role);
    if (email_verified) {
        set += set.empty() ? "SET " : ", ";
        set += "email_verified = ?";
    }
    set += set.empty() ? "SET " : ", ";
    set += "updated_at = ?";

    const std::string sql = "UPDATE users " + set + " WHERE id = ?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        return AuthStatus(AuthErrorCode::kServiceUnavailable, "user update prepare failed");
    }
    // Bind in the SET order: display_name?, role?, email_verified?, updated_at, id.
    int idx = 1;
    for (const auto& t : text_binds) {
        sqlite3_bind_text(stmt, idx++, t.c_str(), -1, SQLITE_TRANSIENT);
    }
    if (email_verified) {
        sqlite3_bind_int(stmt, idx++, *email_verified ? 1 : 0);
    }
    sqlite3_bind_int64(stmt, idx++, NowSec());
    sqlite3_bind_text(stmt, idx++, id.c_str(), -1, SQLITE_TRANSIENT);
    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        return AuthStatus(AuthErrorCode::kServiceUnavailable, "user update failed");
    }
    return Get(id);
}

Result<AdminUserRecord> AdminUsersService::SetStatus(const std::string& id,
                                                     const std::string& status) {
    if (db_ == nullptr) {
        return AuthStatus(AuthErrorCode::kInternalError, "AdminUsersService not initialized");
    }
    // 404 if the user does not exist.
    auto existing = Get(id);
    if (!existing.ok()) return existing.status();

    sqlite3_stmt* stmt = nullptr;
    const char* sql = "UPDATE users SET status = ?, updated_at = ? WHERE id = ?";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return AuthStatus(AuthErrorCode::kServiceUnavailable, "user status prepare failed");
    }
    sqlite3_bind_text(stmt, 1, status.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, NowSec());
    sqlite3_bind_text(stmt, 3, id.c_str(), -1, SQLITE_TRANSIENT);
    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        return AuthStatus(AuthErrorCode::kServiceUnavailable, "user status update failed");
    }
    return Get(id);
}

Result<AdminUserRecord> AdminUsersService::Disable(const std::string& id) {
    return SetStatus(id, "disabled");
}

Result<AdminUserRecord> AdminUsersService::Enable(const std::string& id) {
    return SetStatus(id, "active");
}

}  // namespace cortrix::auth
