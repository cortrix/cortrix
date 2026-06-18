import { API_BASE } from '../utils/constants';
import { mockApi } from './mock';
import { fallbackToMock } from './fallback';

// Health endpoints (P02a design § 4.2, F20-7). Two K8s-style probes:
//   GET /api/v1/system/health/live   — process liveness (always 200 when up)
//   GET /api/v1/system/health/ready  — readiness, 200 when all 5 components are
//                                      ready, 503 (not_ready) while any is down.
// The /ready endpoint returns 503 with a JSON body when not ready, so we read
// the body regardless of HTTP status (a plain get() would throw on 503 and
// drop the component detail we want to render).

export interface LiveResponse {
  status: 'alive';
  uptime_seconds: number;
  version: string;
}

export type ComponentStatus = 'ok' | 'not_ready';

// 5 components per F20-7 § 8.4 (catalog / vector_index / secret_provider /
// spc_pipeline / memory_store). Each may carry component-specific extras
// (bloom_filter_ready, wal_lag_bytes, queue_depth, progress, reason, …) which
// we surface generically as key/value detail rows.
export interface ComponentReadiness {
  status: ComponentStatus;
  reason?: string;
  progress?: number;
  [extra: string]: unknown;
}

export interface ReadyResponse {
  status: 'ready' | 'not_ready';
  uptime_seconds: number;
  components: Record<string, ComponentReadiness>;
  warnings?: string[];
}

export async function getLive(): Promise<LiveResponse> {
  // Production surfaces every failure (HealthPage shows "down"). Standalone falls
  // back to the mock on a 5xx (incl. the dev proxy's down-target 500) / network
  // failure; a 4xx still surfaces. See ./fallback.ts. The status is attached so
  // the gate can classify it (raw fetch does not throw on a non-2xx).
  try {
    const res = await fetch(`${API_BASE}/api/v1/system/health/live`, {
      credentials: 'include',
    });
    if (!res.ok) {
      throw Object.assign(new Error(`live probe failed (${res.status})`), {
        status: res.status,
      });
    }
    return await res.json();
  } catch (e) {
    return fallbackToMock(e, () => mockApi.getLive());
  }
}

export async function getReady(): Promise<ReadyResponse> {
  // Read the body on both 200 (ready) and 503 (not_ready) — the component
  // breakdown lives in the body in either case, so a 503 is NOT an error and
  // parses normally. A 4xx surfaces; production surfaces every other failure too,
  // while a standalone build falls back to the mock (5xx with a non-JSON body, or
  // a network failure → backend unreachable).
  try {
    const res = await fetch(`${API_BASE}/api/v1/system/health/ready`, {
      credentials: 'include',
    });
    if (res.status >= 400 && res.status < 500) {
      throw Object.assign(new Error(`ready probe ${res.status}`), { status: res.status });
    }
    return await res.json();
  } catch (e) {
    return fallbackToMock(e, () => mockApi.getReady());
  }
}
