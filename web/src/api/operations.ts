import { get } from './client';
import { mockApi } from './mock';
import { fallbackToMock } from './fallback';
import type { OperationLogFilter, OperationLogResponse } from '../types/api';

// Operation Log client (operation log CE — GET /api/v1/operations, web UI).
//
// Path note (V5-B4 P0-V5-66): the endpoint is the business prefix
// `/api/v1/operations` — NOT `/api/v1/admin/operations`. Admin cross-user
// queries (user_id != current) trigger the AdminGuard double-protection on the
// backend (operation log); the UI just passes the optional user_id filter.
//
// Mock fallback is build-time gated (./fallback.ts): production surfaces every
// error; only a standalone build exercises the log table + filters against the
// in-memory mock (and a 4xx still surfaces).

const BASE = '/api/v1/operations';

function buildQuery(filter: OperationLogFilter): string {
  const p = new URLSearchParams();
  if (filter.user_id) p.set('user_id', filter.user_id);
  if (filter.action) p.set('action', filter.action);
  if (filter.resource_type) p.set('resource_type', filter.resource_type);
  if (filter.trace_id) p.set('trace_id', filter.trace_id);
  if (filter.from_timestamp != null) p.set('from_timestamp', String(filter.from_timestamp));
  if (filter.to_timestamp != null) p.set('to_timestamp', String(filter.to_timestamp));
  p.set('limit', String(filter.limit ?? 50));
  p.set('offset', String(filter.offset ?? 0));
  return p.toString();
}

export async function listOperations(
  filter: OperationLogFilter = {},
): Promise<OperationLogResponse> {
  try {
    return await get<OperationLogResponse>(`${BASE}?${buildQuery(filter)}`);
  } catch (e) {
    return fallbackToMock(e, () => mockApi.listOperations(filter));
  }
}
