import { create } from 'zustand';
import type { SearchResult, QueryMeta, AgentError } from '../types/api';
import { query } from '../api/query';
import { mockApi } from '../api/mock';
import { USE_MOCK } from '../api/fallback';
import { parseAgentError } from '../api/errors';
import { useAppStore } from './useAppStore';

interface SearchState {
  query: string;
  setQuery: (q: string) => void;

  results: SearchResult[];
  meta: QueryMeta | null;
  /** Structured GEN-Agent error (R4/S2 — rendered inline via ErrorDisplay). */
  error: AgentError | null;

  filters: {
    blockType: string[];
    topK: number;
  };
  setFilters: (f: Partial<SearchState['filters']>) => void;

  isSearching: boolean;
  search: () => Promise<void>;
}

export const useSearchStore = create<SearchState>((set, get) => ({
  query: '',
  setQuery: (q) => set({ query: q }),

  results: [],
  meta: null,
  error: null,

  filters: { blockType: [], topK: 10 },
  setFilters: (f) => set((s) => ({ filters: { ...s.filters, ...f } })),

  isSearching: false,
  search: async () => {
    const { query: q, filters } = get();
    if (!q.trim()) return;
    const ns = useAppStore.getState().currentNamespace;

    set({ isSearching: true, error: null });
    try {
      const res = await query({
        query: q,
        namespaces: [ns],
        top_k: filters.topK,
        // Cross-NS query: the wire field is the singular `filter` (JSONB pass-through);
        // `filters` (the store's local UI slice) stays as-is.
        filter: filters.blockType.length ? { block_type: filters.blockType } : undefined,
      });
      set({ results: res.results, meta: res.meta, error: null });
    } catch (e) {
      // Standalone (D3): a network failure (TypeError) falls back to the mock so
      // search demos work offline; a real backend error is surfaced inline as a
      // structured GEN-Agent error (§ 16.4). The mock is build-time gated
      // (./fallback.ts) so it never runs in a production build.
      if (USE_MOCK && e instanceof TypeError) {
        try {
          const res = await mockApi.search(ns, q);
          set({ results: res.results, meta: res.meta, error: null });
          return;
        } catch {
          /* fall through to error display */
        }
      }
      set({ error: parseAgentError(e), results: [], meta: null });
    } finally {
      set({ isSearching: false });
    }
  },
}));
