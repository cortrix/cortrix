// Standalone (D3) mock-fallback policy — the single place that decides whether a
// thrown API error should surface to the caller or be swallowed in favour of the
// in-memory mock.
//
// Why this exists: in dev the Vite proxy forwards /api to the backend
// (vite.config.ts). When that backend is *down* the proxy does NOT drop the
// connection — it returns an HTTP 500 to the browser. So "no backend" looks like
// a 5xx, not a network failure. That makes a status-blind `catch { return mock }`
// indistinguishable from a real backend error, and a real 4xx (a rejected
// bootstrap token, a 404 namespace, a 403) would get laundered into a fabricated
// mock success — the bug this module fixes.
//
// The rule:
//   - 4xx  → a genuine client/business error. RE-THROW so the page surfaces the
//            real GEN-Agent envelope (CLAUDE.md § 5 — machine-readable errors).
//   - 5xx  → backend unreachable / broken (incl. the dev proxy's down-target 500).
//            Fall back to the mock so the standalone UI stays exercisable.
//   - network failure (TypeError from fetch, no status) → also a fall-back case.

import { ApiError } from './client';

/** Extract an HTTP status from either an ApiError or a `{status}`-tagged Error. */
function statusOf(e: unknown): number | undefined {
  if (e instanceof ApiError) return e.status;
  if (e instanceof Error && 'status' in e) {
    const s = (e as { status: unknown }).status;
    if (typeof s === 'number') return s;
  }
  return undefined;
}

/**
 * True when the error is a real *client* error (HTTP 4xx) that the caller must
 * see — i.e. it should be re-thrown rather than masked by the mock fallback.
 * 5xx and status-less (network) errors return false → fall back to the mock.
 */
export function isClientError(e: unknown): boolean {
  const status = statusOf(e);
  return status !== undefined && status >= 400 && status < 500;
}
