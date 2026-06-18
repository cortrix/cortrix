// Mock-fallback policy — the single chokepoint that decides whether a thrown API
// error surfaces to the caller or is swallowed in favour of the in-memory mock.
//
// Root cause this fixes (Scott R9 #3): the in-memory mock (./mock.ts) was wired
// into the *live* runtime path via a per-call `catch { return mockApi.X() }`. In
// production that masks real backend failures as fabricated successes — most
// dangerously, a rejected /admin/bootstrap handing the caller a fake
// `cortrix_sk_mock` admin key. A status-blind catch can't even distinguish a
// real error from a dev-only outage (the Vite dev proxy returns HTTP 500, not a
// network error, when the backend is down — see vite.config.ts).
//
// The fix is a build-time gate, not a status heuristic:
//
//   USE_MOCK === false  (production build, the default)
//       The mock is OFF. Every API error RE-THROWS and surfaces as a real
//       GEN-Agent envelope (CLAUDE.md § 5). The mock never touches the live
//       path — the anti-pattern is eliminated at the root.
//
//   USE_MOCK === true   (dev / test / `VITE_USE_MOCK=1`)
//       Standalone mode: the UI is exercisable without a live backend. Even
//       here a 4xx (a genuine client/business error) re-throws so demo runs
//       still see real rejections; only a 5xx / network failure (backend
//       unreachable) falls back to the in-memory mock.

import { ApiError } from './client';

// Vite statically replaces import.meta.env.* at build (same access pattern as
// telemetry/metrics.ts). DEV is true under `vite dev` and the Vitest jsdom run;
// a production `vite build` sets it false. VITE_USE_MOCK=1 force-enables the mock
// for an explicit standalone build (e.g. the Playwright E2E webServer).
const ENV: Record<string, unknown> | undefined =
  typeof import.meta !== 'undefined' ? (import.meta as { env?: Record<string, unknown> }).env : undefined;

/**
 * Whether the in-memory mock is allowed to serve as a fallback. OFF in a
 * production build so the mock can never mask a live backend error.
 */
export const USE_MOCK: boolean = ENV?.VITE_USE_MOCK === '1' || ENV?.VITE_USE_MOCK === 'true' || ENV?.DEV === true;

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
 * see — i.e. it must surface rather than be masked by the mock fallback. 5xx and
 * status-less (network) errors return false.
 */
export function isClientError(e: unknown): boolean {
  const status = statusOf(e);
  return status !== undefined && status >= 400 && status < 500;
}

/**
 * Resolve a failed live API call against the in-memory mock, subject to the
 * build-time gate.
 *
 * - USE_MOCK off (production): always re-throw — the mock stays off the live path.
 * - USE_MOCK on (standalone): re-throw a 4xx (real client error), otherwise run
 *   `mockFn` so the UI is exercisable without a backend.
 *
 * Pass the original caught error and a thunk that produces the mock result, e.g.
 *   catch (e) { return fallbackToMock(e, () => mockApi.listUsers(filter)); }
 */
export function fallbackToMock<T>(e: unknown, mockFn: () => T): T {
  if (!USE_MOCK || isClientError(e)) throw e;
  return mockFn();
}
