#pragma once

namespace httplib { class Server; }

namespace cortrix {
class ApiKeyAuth;
namespace auth {
class BootstrapHandler;
class ApiKeyService;
class AdminUsersService;
class JwtSecretService;
}  // namespace auth
namespace observability { class IOperationLogger; }

/// [D3.5 r2 · Wave P · P1] Register the P08-CE bootstrap routes (§2.13.2):
///   GET  /api/v1/admin/bootstrap?token=<t>  -- first-run HTML page (one-time admin key)
///   POST /api/v1/admin/bootstrap  body {token}  -- programmatic JSON variant
///
/// Both share ONE 60s single-use token (V3 ruling 5) owned by `handler`; consuming
/// either mints the admin user + admin API Key and invalidates the token. These are
/// NOT WithAuth-wrapped: on first run no key exists yet, so the bootstrap token IS
/// the credential. AdminGuard (loopback IP filter, /api/v1/admin/* prefix) is the
/// only ambient gate. `handler` must outlive `server`.
void RegisterBootstrapRoutes(httplib::Server& server, auth::BootstrapHandler& handler);

/// [D3.5 r2 · Wave P · P1] Register the P08-CE API Keys resource (§2.13.3):
///   POST   /api/v1/auth/api-keys           -- create a key (plaintext returned once)
///   GET    /api/v1/auth/api-keys           -- list the caller's keys (prefix only)
///   DELETE /api/v1/auth/api-keys/{key_id}  -- revoke a key (soft delete, revoked_at)
///
/// admin permission (WithAuth kPermAdmin). The service persists SHA-256(key) + a
/// prefix in platform.db api_keys; the plaintext is surfaced only at creation. The
/// owning user_id is the caller's AuthContext.user_id (the bootstrap admin in the
/// CE single-user model). `keys` must outlive `server`.
void RegisterApiKeyRoutes(httplib::Server& server, auth::ApiKeyService& keys,
                          ApiKeyAuth& auth);

/// Register the CE Web UI session probe: GET /api/v1/auth/me.
///
/// The JWT email/password /auth/me is a cloud-enterprise endpoint (absent in CE).
/// This CE shim makes the self-hosted Web UI usable out-of-the-box: when auth is
/// DISABLED (the V1 default for "quick experience") it returns 200 with a synthetic
/// single-operator identity so the UI's startup probe authenticates and renders the
/// app shell instead of dead-ending on a login wall whose backend is cloud-enterprise.
/// When auth is ENABLED it returns 401 (UI shows login; full CE auth-on Web UI flow is
/// a V1.5 multi-user item). NOT WithAuth-wrapped — this is the pre-auth probe.
void RegisterAuthSessionRoute(httplib::Server& server, ApiKeyAuth& auth);

/// [FA1 R11] Register the P08 §2.13-bis admin/users 5 endpoints (admin role only):
///   GET    /api/v1/admin/users              -- list (q / status / role / page / limit)
///   POST   /api/v1/admin/users              -- create (invite mode, no email verify)
///   PATCH  /api/v1/admin/users/:id          -- update (display_name / role / email_verified)
///   POST   /api/v1/admin/users/:id/disable  -- disable (active -> disabled, soft)
///   POST   /api/v1/admin/users/:id/enable   -- enable (disabled -> active)
///
/// All WithAuth(kPermAdmin) (+ AdminGuard loopback IP layer). The 4 mutating
/// endpoints write the user.created / user.updated / user.disabled / user.enabled
/// operation_log actions through `logger` (may be null = no-op, e.g. in tests).
/// Errors are the GEN-Agent body with the §2.13-bis statuses (404/409/422/400).
/// `users` (and `logger` when non-null) must outlive `server`.
void RegisterAdminUsersRoutes(httplib::Server& server, auth::AdminUsersService& users,
                              ApiKeyAuth& auth,
                              observability::IOperationLogger* logger = nullptr);

/// [FA1 R11] Register the P08 §2.11 JWT secret rotation endpoint (topic 1.1 C):
///   POST /api/v1/admin/auth/rotate-jwt-secret  -- rotate HS256 secret (dual-key window)
///
/// WithAuth(kPermAdmin) (+ AdminGuard). Returns the Agent-friendly RotationResult
/// ({rotated, new_key_id, prev_key_id, prev_expires_at, warning}). `jwt` must
/// outlive `server`.
void RegisterJwtRotateRoute(httplib::Server& server, auth::JwtSecretService& jwt,
                            ApiKeyAuth& auth);

}  // namespace cortrix
