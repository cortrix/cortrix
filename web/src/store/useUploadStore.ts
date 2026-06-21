import { create } from 'zustand';
import type { UploadFile } from '../types/state';
import type { DocumentStatus, BatchSubmitResponse, AgentError } from '../types/api';
import { uploadDocument, getDocumentStatus, listDocuments, deleteDocument } from '../api/documents';
import { batchSubmit } from '../api/batch';
import { parseAgentError } from '../api/errors';
import { ensureCurrentNamespace, useAppStore } from './useAppStore';

interface UploadState {
  files: UploadFile[];
  documents: DocumentStatus[];
  documentsLoading: boolean;
  // Bulk submit (TD-F42-BULK-SUBMIT) — partial-success result + BATCH-level error.
  batchResult: BatchSubmitResponse | null;
  batchError: AgentError | null;
  batchSubmitting: boolean;
  addFiles: (files: File[]) => void;
  removeFile: (id: string) => void;
  uploadFile: (uf: UploadFile) => Promise<void>;
  pollProcessing: () => Promise<void>;
  loadDocuments: () => Promise<void>;
  deleteDoc: (docId: number) => Promise<void>;
  /** Submit a JSON-defined batch of documents (1–100); sets batchResult/Error. */
  submitBatch: (docsJson: string) => Promise<void>;
  clearBatch: () => void;
}

export const useUploadStore = create<UploadState>((set, get) => ({
  files: [],
  documents: [],
  documentsLoading: false,
  batchResult: null,
  batchError: null,
  batchSubmitting: false,

  addFiles: (newFiles) => {
    const uploads: UploadFile[] = newFiles.map((f) => ({
      id: `upload-${Date.now()}-${Math.random().toString(36).slice(2, 8)}`,
      file: f,
      progress: 0,
      status: 'pending',
    }));
    set((s) => ({ files: [...uploads, ...s.files] }));
    uploads.forEach((uf) => get().uploadFile(uf));
  },

  removeFile: (id) => set((s) => ({ files: s.files.filter((f) => f.id !== id) })),

  uploadFile: async (uf) => {
    const ns = await ensureCurrentNamespace();
    if (!ns) {
      set((s) => ({
        files: s.files.map((f) =>
          f.id === uf.id ? { ...f, status: 'error' as const, error: 'No namespace available' } : f,
        ),
      }));
      return;
    }
    set((s) => ({
      files: s.files.map((f) => (f.id === uf.id ? { ...f, status: 'uploading' as const } : f)),
    }));

    try {
      const res = await uploadDocument(ns, uf.file, (pct) => {
        set((s) => ({
          files: s.files.map((f) => (f.id === uf.id ? { ...f, progress: pct } : f)),
        }));
      });
      set((s) => ({
        files: s.files.map((f) =>
          f.id === uf.id
            ? { ...f, doc_id: res.doc_id, progress: 100, status: 'processing' as const }
            : f,
        ),
      }));
    } catch (e) {
      set((s) => ({
        files: s.files.map((f) =>
          f.id === uf.id ? { ...f, status: 'error' as const, error: String(e) } : f,
        ),
      }));
    }
  },

  pollProcessing: async () => {
    const ns = await ensureCurrentNamespace();
    if (!ns) return;
    const processing = get().files.filter((f) => f.status === 'processing' && f.doc_id);
    await Promise.all(
      processing.map(async (f) => {
        try {
          const docStatus = await getDocumentStatus(ns, f.doc_id!);
          if (docStatus.status === 'ready' || docStatus.status === 'error') {
            set((s) => ({
              files: s.files.map((file) =>
                file.id === f.id
                  ? {
                      ...file,
                      status: docStatus.status as UploadFile['status'],
                      error: docStatus.error_message,
                    }
                  : file,
              ),
            }));
            // Refresh document list when processing completes
            get().loadDocuments();
          }
        } catch {
          // ignore polling errors
        }
      }),
    );
  },

  loadDocuments: async () => {
    const ns = await ensureCurrentNamespace();
    if (!ns) {
      set({ documents: [], documentsLoading: false });
      return;
    }
    set({ documentsLoading: true });
    try {
      const data = await listDocuments(ns);
      set({ documents: data.documents || [], documentsLoading: false });
    } catch {
      set({ documentsLoading: false });
    }
  },

  deleteDoc: async (docId) => {
    const ns = await ensureCurrentNamespace();
    if (!ns) return;
    try {
      await deleteDocument(ns, docId);
      set((s) => ({
        documents: s.documents.filter((d) => d.doc_id !== docId),
      }));
    } catch (e) {
      useAppStore.getState().setGlobalError?.(`Delete failed: ${e}`);
    }
  },

  // Bulk submit (TD-F42-BULK-SUBMIT). The textarea holds a JSON array of
  // `{ doc_id, content, metadata? }`. A parse error / BATCH-level failure
  // (empty, >100, duplicate) lands in `batchError`; otherwise the per-doc
  // partial-success envelope lands in `batchResult` (succeeded[] / failed[]).
  submitBatch: async (docsJson) => {
    const ns = await ensureCurrentNamespace();
    if (!ns) {
      set({
        batchSubmitting: false,
        batchError: {
          code: 'CX_ERR_NS_UNAVAILABLE',
          message: 'No namespace available.',
          retryable: false,
          category: 'permanent',
          retry_after_ms: null,
        },
      });
      return;
    }
    set({ batchSubmitting: true, batchResult: null, batchError: null });
    let documents: unknown;
    try {
      documents = JSON.parse(docsJson);
    } catch {
      set({
        batchSubmitting: false,
        batchError: {
          code: 'CX_ERR_BATCH_INVALID_JSON',
          message: 'Documents must be a valid JSON array.',
          retryable: false,
          category: 'permanent',
          retry_after_ms: null,
        },
      });
      return;
    }
    if (!Array.isArray(documents)) {
      set({
        batchSubmitting: false,
        batchError: {
          code: 'CX_ERR_BATCH_INVALID_JSON',
          message: 'Documents must be a JSON array of { doc_id, content }.',
          retryable: false,
          category: 'permanent',
          retry_after_ms: null,
        },
      });
      return;
    }
    try {
      const res = await batchSubmit({ namespace: ns, documents: documents as never, options: { async: true } });
      set({ batchResult: res, batchSubmitting: false });
      // Refresh the document list so accepted docs surface in the table.
      void get().loadDocuments();
    } catch (e) {
      set({ batchError: parseAgentError(e), batchSubmitting: false });
    }
  },

  clearBatch: () => set({ batchResult: null, batchError: null }),
}));
