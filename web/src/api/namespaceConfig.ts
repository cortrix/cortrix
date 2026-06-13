import { get, post, put } from './client';
import { mockApi } from './mock';
import type {
  NamespaceDetail,
  CreateNamespaceFullRequest,
  UpdateNamespaceRequest,
} from '../types/api';

// F12 Namespace CRUD client — the config-rich operations that complement the
// simple list/create/delete in namespaces.ts:
//   GET  /api/v1/namespaces/{name}         get detail (11 *_config + admission)
//   POST /api/v1/namespaces                create with description + configs
//   PUT  /api/v1/namespaces/{name}         update description / visibility / configs
//
// Standalone discipline (D3): targets frozen contract endpoints, falls back to
// the in-memory mock when the backend is unreachable. Delete stays in
// namespaces.ts (soft delete, status='deleted', F12 § 3.1).

const BASE = '/api/v1/namespaces';

export async function getNamespaceDetail(name: string): Promise<NamespaceDetail> {
  try {
    return await get<NamespaceDetail>(`${BASE}/${encodeURIComponent(name)}`);
  } catch {
    return mockApi.getNamespaceDetail(name);
  }
}

export async function createNamespaceFull(
  req: CreateNamespaceFullRequest,
): Promise<NamespaceDetail> {
  try {
    return await post<NamespaceDetail>(BASE, req);
  } catch {
    return mockApi.createNamespaceFull(req);
  }
}

export async function updateNamespace(
  name: string,
  req: UpdateNamespaceRequest,
): Promise<NamespaceDetail> {
  try {
    return await put<NamespaceDetail>(`${BASE}/${encodeURIComponent(name)}`, req);
  } catch {
    return mockApi.updateNamespace(name, req);
  }
}
