import { get, post, patch, del } from './client';
import { mockApi } from './mock';
import type {
  MemoryListFilter,
  MemoryListResponse,
  MemoryCreateRequest,
  MemoryCreateResponse,
  MemoryEditRequest,
  MemoryEditResponse,
  MemoryInvalidateResponse,
} from '../types/api';

// MEM03 Memory Transparency client (4 endpoints, P02a design § 7.2):
//   GET    /api/v1/memory?explain={bool}   list
//   POST   /api/v1/memory                  create
//   PATCH  /api/v1/memory/{id}             edit (server sets extraction_method=user_edit)
//   DELETE /api/v1/memory/{id}             invalidate (soft delete — NOT a hard delete)
//
// Standalone discipline (D3): each call targets the frozen contract endpoint but
// falls back to the in-memory mock when the backend is unreachable, so the UI is
// fully exercisable without a live server. Cross-feature wiring (content
// materialization, real auth) is deferred to D3.5.

const BASE = '/api/v1/memory';

function buildQuery(filter: MemoryListFilter): string {
  const params = new URLSearchParams();
  params.set('user_id', filter.user_id);
  if (filter.include_invalidated) params.set('include_invalidated', 'true');
  if (filter.memory_type) params.set('memory_type', filter.memory_type);
  if (filter.explain) params.set('explain', 'true');
  params.set('page', String(filter.page ?? 0));
  params.set('page_size', String(filter.page_size ?? 20));
  return params.toString();
}

export async function listMemory(filter: MemoryListFilter): Promise<MemoryListResponse> {
  try {
    return await get<MemoryListResponse>(`${BASE}?${buildQuery(filter)}`);
  } catch {
    return mockApi.listMemory(filter);
  }
}

export async function createMemory(req: MemoryCreateRequest): Promise<MemoryCreateResponse> {
  try {
    return await post<MemoryCreateResponse>(BASE, req);
  } catch {
    return mockApi.createMemory(req);
  }
}

export async function editMemory(
  id: string,
  req: MemoryEditRequest,
): Promise<MemoryEditResponse> {
  try {
    return await patch<MemoryEditResponse>(`${BASE}/${encodeURIComponent(id)}`, req);
  } catch {
    return mockApi.editMemory(id, req);
  }
}

/** Soft delete (invalidate). HTTP DELETE verb; semantic = memory_invalidate. */
export async function invalidateMemory(id: string): Promise<MemoryInvalidateResponse> {
  try {
    return await del<MemoryInvalidateResponse>(`${BASE}/${encodeURIComponent(id)}`);
  } catch {
    return mockApi.invalidateMemory(id);
  }
}
