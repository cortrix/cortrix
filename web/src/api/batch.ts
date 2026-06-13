import { post } from './client';
import { mockApi } from './mock';
import type { BatchSubmitRequest, BatchSubmitResponse } from '../types/api';

// Bulk document submit (TD-F42-BULK-SUBMIT):
//   POST /api/v1/documents/batch-submit  — 1–100 docs, partial-success schema.
//
// BATCH-level failures (empty / size / payload-too-large / duplicate doc_id)
// surface as a thrown ApiError (parsed via parseAgentError); per-doc failures
// come back in `meta.failed[]` with the GEN-Agent 5 fields. Standalone (D3):
// falls back to the in-memory mock when the backend is unreachable, but a
// BATCH-level validation error from the mock is rethrown (not swallowed).

export async function batchSubmit(req: BatchSubmitRequest): Promise<BatchSubmitResponse> {
  try {
    return await post<BatchSubmitResponse>('/api/v1/documents/batch-submit', req);
  } catch (err) {
    // Network/unreachable → mock. A real 4xx (BATCH-level) is rethrown so the
    // UI can show the structured error rather than a misleading mock success.
    const status = (err as { status?: number }).status;
    if (typeof status === 'number' && status >= 400 && status < 500) throw err;
    return mockApi.batchSubmit(req);
  }
}
