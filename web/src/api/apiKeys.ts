import { get, post, del } from './client';
import { mockApi } from './mock';
import type { ApiKeySummary, ApiKeyCreateRequest, ApiKeyCreateResponse } from '../types/api';

// API Keys client (P08 § 2.13.3 — user-level keys for SDK / MCP, P02a § 9.3):
//   GET    /api/v1/auth/api-keys           list (no plaintext key)
//   POST   /api/v1/auth/api-keys           create (plaintext `key` returned ONCE)
//   DELETE /api/v1/auth/api-keys/{id}      revoke
//
// Standalone discipline (D3): falls back to the in-memory mock when the backend
// is unreachable so the Settings -> API Keys section is exercisable offline.

const BASE = '/api/v1/auth/api-keys';

export async function listApiKeys(): Promise<ApiKeySummary[]> {
  try {
    return await get<ApiKeySummary[]>(BASE);
  } catch {
    return mockApi.listApiKeys();
  }
}

export async function createApiKey(req: ApiKeyCreateRequest): Promise<ApiKeyCreateResponse> {
  try {
    return await post<ApiKeyCreateResponse>(BASE, req);
  } catch {
    return mockApi.createApiKey(req);
  }
}

export async function revokeApiKey(id: string): Promise<void> {
  try {
    await del(`${BASE}/${encodeURIComponent(id)}`);
  } catch {
    mockApi.revokeApiKey(id);
  }
}
