import { post } from './client';
import { mockApi } from './mock';
import type { AuthMeResponse, LoginResponse, BootstrapResponse } from '../types/api';

// Auth client (P08 / P02a § 9.2 — HttpOnly cookie model). All calls send
// credentials:'include' via the shared client; mutations also carry the
// X-CSRF-Token header (§ 4.6). The token itself is never read or stored by JS.
//
// Standalone discipline (D3): each call targets the frozen contract endpoint
// but falls back to the in-memory mock session when the backend is unreachable,
// so the auth flow (bootstrap → login → guarded pages) is fully exercisable
// without a live server. Real Set-Cookie wiring lands at D3.5.

/** GET /api/v1/auth/me — cookie probe. Returns null when unauthenticated. */
export async function fetchMe(): Promise<AuthMeResponse | null> {
  try {
    const res = await fetch('/api/v1/auth/me', { credentials: 'include' });
    if (res.status === 401 || res.status === 403) return null;
    if (!res.ok) {
      throw Object.assign(new Error(`auth/me ${res.status}`), { status: res.status });
    }
    return (await res.json()) as AuthMeResponse;
  } catch (e) {
    // Only fall back to the mock session for network failures, not for an
    // explicit backend HTTP status — a real 5xx must surface, not be masked
    // as an authenticated session.
    if (e instanceof Error && 'status' in e) throw e;
    return mockApi.authMe();
  }
}

/** POST /api/v1/auth/login — Web UI path (Set-Cookie + user body, no token). */
export async function login(email: string, password: string): Promise<AuthMeResponse> {
  try {
    const res = await fetch('/api/v1/auth/login', {
      method: 'POST',
      credentials: 'include',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ email, password }),
    });
    if (!res.ok) {
      const body = await res.text().catch(() => '');
      // Surface backend GEN-Agent error envelope to the caller.
      throw Object.assign(new Error(body || `login ${res.status}`), { status: res.status });
    }
    const data = (await res.json()) as LoginResponse;
    return data.user;
  } catch (e) {
    // Only fall back to mock for network failures, not for explicit 4xx.
    if (e instanceof Error && 'status' in e) throw e;
    return mockApi.login(email, password);
  }
}

/** POST /api/v1/auth/logout — backend clears the auth + csrf cookies. */
export async function logout(): Promise<void> {
  // Logout is best-effort: the caller clears local UI state regardless, so a
  // backend error must never strand the user "logged in". We only touch the
  // mock session on a network failure (standalone); an HTTP error from a live
  // backend is swallowed silently rather than mutating the mock.
  try {
    await fetch('/api/v1/auth/logout', { method: 'POST', credentials: 'include' });
  } catch {
    mockApi.logout();
  }
}

/**
 * POST /api/v1/admin/bootstrap { token } — programmatic one-time exchange of a
 * bootstrap token for the admin API key (P08 § 2.13.2.b). The token is consumed
 * on first use by either the GET (browser) or POST (this) endpoint.
 */
export async function bootstrap(token: string): Promise<BootstrapResponse> {
  try {
    return await post<BootstrapResponse>('/api/v1/admin/bootstrap', { token });
  } catch (e) {
    // Security: only synthesise a mock admin key when the backend is genuinely
    // unreachable (network failure). An explicit HTTP status — e.g. a 4xx for an
    // invalid or already-consumed token — MUST surface, otherwise a failed
    // exchange would hand the caller a fabricated `cortrix_sk_mock` admin key.
    if (e instanceof Error && 'status' in e) throw e;
    return mockApi.bootstrap(token);
  }
}
