import { post } from './client';
import { mockApi } from './mock';
import { USE_MOCK, fallbackToMock } from './fallback';
import type { AuthMeResponse, LoginResponse, BootstrapResponse } from '../types/api';

// Auth client (auth / web UI — HttpOnly cookie model). All calls send
// credentials:'include' via the shared client; mutations also carry the
// X-CSRF-Token header (§ 4.6). The token itself is never read or stored by JS.
//
// Mock fallback is build-time gated (see ./fallback.ts): in production the mock
// is OFF and every error surfaces — what stops e.g. a rejected bootstrap token
// from minting a fabricated admin key. Only a standalone build (dev / test /
// VITE_USE_MOCK) falls back to the in-memory session, and even then a 4xx still
// surfaces. Real Set-Cookie wiring lands at D3.5.

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
    // Standalone: a 5xx / network failure defers to the mock session so the app
    // shell still boots; a 4xx surfaces. Production: surfaces everything.
    return fallbackToMock(e, () => mockApi.authMe());
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
    // An explicit credential submit must never silently mock-authenticate when
    // the backend actually answered: any HTTP status (4xx or 5xx) surfaces.
    // Only a network failure under a standalone build falls back to the mock.
    if (!USE_MOCK || (e instanceof Error && 'status' in e)) throw e;
    return mockApi.login(email, password);
  }
}

/** POST /api/v1/auth/logout — backend clears the auth + csrf cookies. */
export async function logout(): Promise<void> {
  // Logout is best-effort: the caller clears local UI state regardless, so a
  // backend error must never strand the user "logged in". Only a standalone
  // build mirrors the logout into the mock session on a network failure.
  try {
    await fetch('/api/v1/auth/logout', { method: 'POST', credentials: 'include' });
  } catch {
    if (USE_MOCK) mockApi.logout();
  }
}

/**
 * POST /api/v1/admin/bootstrap { token } — programmatic one-time exchange of a
 * bootstrap token for the admin API key (auth). The token is consumed
 * on first use by either the GET (browser) or POST (this) endpoint.
 */
export async function bootstrap(token: string): Promise<BootstrapResponse> {
  try {
    return await post<BootstrapResponse>('/api/v1/admin/bootstrap', { token });
  } catch (e) {
    // Security: in production the mock is off so any failed exchange surfaces —
    // it can never hand back a fabricated `cortrix_sk_mock` admin key. Even in a
    // standalone build a 4xx (invalid / consumed / expired token) surfaces.
    return fallbackToMock(e, () => mockApi.bootstrap(token));
  }
}
