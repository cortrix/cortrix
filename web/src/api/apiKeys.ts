import { get, post, del } from './client';
import { mockApi } from './mock';
import { fallbackToMock } from './fallback';
import type { ApiKeySummary, ApiKeyCreateRequest, ApiKeyCreateResponse } from '../types/api';

// API Keys client (auth — user-level keys for SDK / MCP, web UI):
//   GET    /api/v1/auth/api-keys           list (no plaintext key)
//   POST   /api/v1/auth/api-keys           create (plaintext `key` returned ONCE)
//   DELETE /api/v1/auth/api-keys/{id}      revoke
//
// Mock fallback is build-time gated (./fallback.ts): production surfaces every
// error; only a standalone build exercises the Settings -> API Keys section
// against the in-memory mock (and a 4xx still surfaces).

const BASE = '/api/v1/auth/api-keys';

export async function listApiKeys(): Promise<ApiKeySummary[]> {
  try {
    return await get<ApiKeySummary[]>(BASE);
  } catch (e) {
    return fallbackToMock(e, () => mockApi.listApiKeys());
  }
}

export async function createApiKey(req: ApiKeyCreateRequest): Promise<ApiKeyCreateResponse> {
  try {
    return await post<ApiKeyCreateResponse>(BASE, req);
  } catch (e) {
    return fallbackToMock(e, () => mockApi.createApiKey(req));
  }
}

export async function revokeApiKey(id: string): Promise<void> {
  try {
    await del(`${BASE}/${encodeURIComponent(id)}`);
  } catch (e) {
    fallbackToMock(e, () => mockApi.revokeApiKey(id));
  }
}
