import { get, post, patch, del } from './client';
import { mockApi } from './mock';
import { fallbackToMock } from './fallback';
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
// Mock fallback is build-time gated (./fallback.ts): production surfaces every
// error; only a standalone build falls back to the in-memory mock (and a 4xx
// still surfaces). Cross-feature wiring (content materialization, real auth) is
// deferred to D3.5.

const BASE = '/api/v1/memory';

function buildQuery(filter: MemoryListFilter): string {
  const params = new URLSearchParams();
  params.set('namespace', filter.namespace);
  if (filter.user_id) params.set('user_id', filter.user_id);
  if (filter.include_invalidated) params.set('include_invalidated', 'true');
  if (filter.memory_type) params.set('memory_type', filter.memory_type);
  if (filter.explain) params.set('explain', 'true');
  const limit = filter.limit ?? filter.page_size ?? 20;
  const offset = filter.offset ?? (filter.page ?? 0) * limit;
  params.set('limit', String(limit));
  params.set('offset', String(offset));
  return params.toString();
}

export async function listMemory(filter: MemoryListFilter): Promise<MemoryListResponse> {
  try {
    const data = await get<MemoryListResponse & { total?: number }>(`${BASE}?${buildQuery(filter)}`);
    if (data.meta) return data;
    const limit = filter.limit ?? filter.page_size ?? 20;
    const offset = filter.offset ?? (filter.page ?? 0) * limit;
    return {
      memories: data.memories ?? [],
      meta: {
        total: data.total ?? data.memories?.length ?? 0,
        page: Math.floor(offset / limit),
        page_size: limit,
      },
    };
  } catch (e) {
    return fallbackToMock(e, () => mockApi.listMemory(filter));
  }
}

export async function createMemory(req: MemoryCreateRequest): Promise<MemoryCreateResponse> {
  try {
    return await post<MemoryCreateResponse>(BASE, req);
  } catch (e) {
    return fallbackToMock(e, () => mockApi.createMemory(req));
  }
}

export async function editMemory(
  id: string,
  req: MemoryEditRequest,
): Promise<MemoryEditResponse> {
  try {
    return await patch<MemoryEditResponse>(`${BASE}/${encodeURIComponent(id)}`, req);
  } catch (e) {
    return fallbackToMock(e, () => mockApi.editMemory(id, req));
  }
}

/** Soft delete (invalidate). HTTP DELETE verb; semantic = memory_invalidate. */
export async function invalidateMemory(id: string): Promise<MemoryInvalidateResponse> {
  try {
    return await del<MemoryInvalidateResponse>(`${BASE}/${encodeURIComponent(id)}`);
  } catch (e) {
    return fallbackToMock(e, () => mockApi.invalidateMemory(id));
  }
}
