import { uploadFile, get, del } from './client';
import type { UploadDocumentResponse, DocumentStatus } from '../types/api';

export async function uploadDocument(
  namespace: string,
  file: File,
  onProgress?: (percentage: number) => void,
): Promise<UploadDocumentResponse> {
  const path = `/api/v1/namespaces/${namespace}/documents`;
  const response = await uploadFile(path, file, onProgress);
  return response.json();
}

export async function getDocumentStatus(
  namespace: string,
  docId: number,
): Promise<DocumentStatus> {
  return get<DocumentStatus>(`/api/v1/namespaces/${namespace}/documents/${docId}/status`);
}

export interface DocumentListResponse {
  documents: DocumentStatus[];
  total_count: number;
}

export async function listDocuments(
  namespace: string,
  limit = 50,
  offset = 0,
): Promise<DocumentListResponse> {
  return get<DocumentListResponse>(
    `/api/v1/namespaces/${namespace}/documents?limit=${limit}&offset=${offset}`,
  );
}

export async function deleteDocument(
  namespace: string,
  docId: number,
): Promise<void> {
  await del(`/api/v1/namespaces/${namespace}/documents/${docId}`);
}
