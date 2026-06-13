import { get } from './client';
import { mockApi } from './mock';
import type { OperationLogFilter, OperationLogResponse } from '../types/api';

// Operation Log client (F18a CE — GET /api/v1/operations, P02a § 9-bis.2).
//
// Path note (V5-B4 P0-V5-66): the endpoint is the business prefix
// `/api/v1/operations` — NOT `/api/v1/admin/operations`. Admin cross-user
// queries (user_id != current) trigger the AdminGuard double-protection on the
// backend (F18a § 6.1); the UI just passes the optional user_id filter.
//
// Standalone discipline (D3): falls back to the in-memory mock when the backend
// is unreachable so the log table + filters are exercisable without a server.

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
  } catch {
    return mockApi.listOperations(filter);
  }
}
