import { get, post, del } from './client';
import type {
  NamespaceInfo,
  NamespaceListResponse,
  CreateNamespaceRequest,
  CreateNamespaceResponse,
} from '../types/api';

export async function listNamespaces(): Promise<NamespaceInfo[]> {
  const response = await get<NamespaceListResponse>('/api/v1/namespaces');
  return response.namespaces;
}

export async function createNamespace(name: string): Promise<CreateNamespaceResponse> {
  const request: CreateNamespaceRequest = { name };
  return post<CreateNamespaceResponse>('/api/v1/namespaces', request);
}

export async function deleteNamespace(name: string): Promise<void> {
  await del<void>(`/api/v1/namespaces/${name}`);
}
