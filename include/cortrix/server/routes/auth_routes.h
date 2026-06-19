#pragma once

namespace httplib { class Server; }

namespace cortrix {
class ApiKeyAuth;
namespace auth {
class BootstrapHandler;
class ApiKeyService;
}  // namespace auth

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

}  // namespace cortrix
