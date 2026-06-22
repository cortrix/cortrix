import { post } from './client';
import { mockApi } from './mock';
import { fallbackToMock } from './fallback';
import type { BatchSubmitRequest, BatchSubmitResponse } from '../types/api';

// Bulk document submit (TD-F42-BULK-SUBMIT):
//   POST /api/v1/documents/batch  — 1–100 docs, partial-success schema.
//
// BATCH-level failures (empty / size / payload-too-large / duplicate doc_id)
// surface as a thrown ApiError (parsed via parseAgentError); per-doc failures
// come back in `meta.failed[]` with the GEN-Agent 5 fields. Mock fallback is
// build-time gated (./fallback.ts): production surfaces every error; a standalone
// build falls back to the mock, but a BATCH-level 4xx still surfaces.

export async function batchSubmit(req: BatchSubmitRequest): Promise<BatchSubmitResponse> {
  try {
    return await post<BatchSubmitResponse>('/api/v1/documents/batch', req);
  } catch (e) {
    return fallbackToMock(e, () => mockApi.batchSubmit(req));
  }
}
