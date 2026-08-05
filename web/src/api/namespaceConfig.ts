import { get, post, put } from './client';
import { mockApi } from './mock';
import { fallbackToMock } from './fallback';
import type {
  NamespaceDetail,
  CreateNamespaceFullRequest,
  UpdateNamespaceRequest,
} from '../types/api';

// Catalog Namespace CRUD client — the config-rich operations that complement the
// simple list/create/delete in namespaces.ts:
//   GET  /api/v1/namespaces/{name}         get detail (11 *_config + admission)
//   POST /api/v1/namespaces                create with description + configs
//   PUT  /api/v1/namespaces/{name}         update description / visibility / configs
//
// Mock fallback is build-time gated (./fallback.ts): production surfaces every
// error (a 404 unknown namespace / 403 forbidden / 409 namespace pool admission reaches the
// page); only a standalone build falls back to the in-memory mock, and even then
// a 4xx still surfaces. Delete stays in namespaces.ts (soft delete, catalog).

const BASE = '/api/v1/namespaces';

export async function getNamespaceDetail(name: string): Promise<NamespaceDetail> {
  try {
    return await get<NamespaceDetail>(`${BASE}/${encodeURIComponent(name)}`);
  } catch (e) {
    return fallbackToMock(e, () => mockApi.getNamespaceDetail(name));
  }
}

export async function createNamespaceFull(
  req: CreateNamespaceFullRequest,
): Promise<NamespaceDetail> {
  try {
    return await post<NamespaceDetail>(BASE, req);
  } catch (e) {
    // Namespace pool admission errors (4xx) must reach AdmissionError on the page.
    return fallbackToMock(e, () => mockApi.createNamespaceFull(req));
  }
}

export async function updateNamespace(
  name: string,
  req: UpdateNamespaceRequest,
): Promise<NamespaceDetail> {
  try {
    return await put<NamespaceDetail>(`${BASE}/${encodeURIComponent(name)}`, req);
  } catch (e) {
    return fallbackToMock(e, () => mockApi.updateNamespace(name, req));
  }
}
