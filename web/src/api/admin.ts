import { get, post, patch } from './client';
import { mockApi } from './mock';
import { fallbackToMock } from './fallback';
import type {
  UserListFilter,
  UserListResponse,
  UserCreateRequest,
  UserUpdateRequest,
  UserMutationResponse,
} from '../types/api';

// Admin / Users client (P08 § 2.13-bis — 5 endpoints, admin role only):
//   GET    /api/v1/admin/users              list (q / status / role / page / limit)
//   POST   /api/v1/admin/users              create (invite mode, no email verify)
//   PATCH  /api/v1/admin/users/:id          update (display_name / role / email_verified)
//   POST   /api/v1/admin/users/:id/disable  disable (active -> disabled, no delete)
//   POST   /api/v1/admin/users/:id/enable   enable (disabled -> active)
//
// Note (P02a-9bis-3 / V6 Stage 3 A30): the 5 endpoints are a disable+enable
// pair, NOT a hard delete — disabled users are retained. Mock fallback is
// build-time gated (./fallback.ts): production surfaces every error; only a
// standalone build falls back to the in-memory mock (and a 4xx still surfaces).

const BASE = '/api/v1/admin/users';

function buildQuery(filter: UserListFilter): string {
  const p = new URLSearchParams();
  if (filter.q) p.set('q', filter.q);
  if (filter.status) p.set('status', filter.status);
  if (filter.role) p.set('role', filter.role);
  p.set('page', String(filter.page ?? 1));
  p.set('limit', String(filter.limit ?? 20));
  return p.toString();
}

export async function listUsers(filter: UserListFilter = {}): Promise<UserListResponse> {
  try {
    return await get<UserListResponse>(`${BASE}?${buildQuery(filter)}`);
  } catch (e) {
    return fallbackToMock(e, () => mockApi.listUsers(filter));
  }
}

export async function createUser(req: UserCreateRequest): Promise<UserMutationResponse> {
  try {
    return await post<UserMutationResponse>(BASE, req);
  } catch (e) {
    return fallbackToMock(e, () => mockApi.createUser(req));
  }
}

export async function updateUser(id: string, req: UserUpdateRequest): Promise<UserMutationResponse> {
  try {
    return await patch<UserMutationResponse>(`${BASE}/${encodeURIComponent(id)}`, req);
  } catch (e) {
    return fallbackToMock(e, () => mockApi.updateUser(id, req));
  }
}

export async function disableUser(id: string): Promise<UserMutationResponse> {
  try {
    return await post<UserMutationResponse>(`${BASE}/${encodeURIComponent(id)}/disable`);
  } catch (e) {
    return fallbackToMock(e, () => mockApi.setUserStatus(id, 'disabled'));
  }
}

export async function enableUser(id: string): Promise<UserMutationResponse> {
  try {
    return await post<UserMutationResponse>(`${BASE}/${encodeURIComponent(id)}/enable`);
  } catch (e) {
    return fallbackToMock(e, () => mockApi.setUserStatus(id, 'active'));
  }
}
