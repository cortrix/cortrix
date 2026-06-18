import { create } from 'zustand';
import type { NamespaceInfo, SystemStatus } from '../types/api';
import { listNamespaces } from '../api/namespaces';
import { get } from '../api/client';

interface HealthResponse {
  version: string;
  status: string;
  uptime_seconds: number;
  llm_enabled: boolean;
  llm_provider: string;
  llm_model: string;
}

// Note (R3/S5): page navigation moved from this store's `activePage` field to
// the URL (react-router-dom, see src/routes.tsx). The store no longer tracks
// the active page — Sidebar/Header use <NavLink> / useNavigate instead.

// currentNamespace is persisted to localStorage (same hand-rolled pattern as the
// theme toggle below — this store deliberately uses no zustand persist
// middleware). Without it a full reload / remount reverted the selection to
// 'default', losing the user's working namespace across chat / search / upload.
const NAMESPACE_KEY = 'cortrix-namespace';

function initialNamespace(): string {
  if (typeof window === 'undefined') return 'default';
  const stored = localStorage.getItem(NAMESPACE_KEY);
  return stored && stored.trim() ? stored : 'default';
}

interface AppState {
  currentNamespace: string;
  setCurrentNamespace: (ns: string) => void;

  namespaces: NamespaceInfo[];
  loadNamespaces: () => Promise<void>;

  systemStatus: SystemStatus | null;
  loadSystemStatus: () => Promise<void>;

  globalError: string | null;
  setGlobalError: (error: string | null) => void;

  theme: 'light' | 'dark';
  toggleTheme: () => void;
}

export const useAppStore = create<AppState>((set, _get) => ({
  currentNamespace: initialNamespace(),
  setCurrentNamespace: (ns) => {
    if (typeof window !== 'undefined') localStorage.setItem(NAMESPACE_KEY, ns);
    set({ currentNamespace: ns });
  },

  namespaces: [],
  loadNamespaces: async () => {
    try {
      const ns = await listNamespaces();
      set({ namespaces: ns });
    } catch (e) {
      set({ globalError: `Failed to load namespaces: ${e}` });
    }
  },

  systemStatus: null,
  loadSystemStatus: async () => {
    try {
      const health = await get<HealthResponse>('/api/v1/health');
      const nsList = _get().namespaces;
      const totalDocs = nsList.reduce((sum, ns) => sum + ns.doc_count, 0);
      const totalBlocks = nsList.reduce((sum, ns) => sum + ns.block_count, 0);

      // Also check cortrix-agent for LLM status (it has the actual LLM config)
      let llmProvider = health.llm_provider || '';
      let llmEnabled = health.llm_enabled;
      try {
        const agentConfig = await get<{ llm_provider: string; llm_model: string; cortrix_connected: boolean }>('/agent/config');
        if (agentConfig.llm_provider && agentConfig.llm_provider !== 'mock') {
          llmEnabled = true;
          llmProvider = agentConfig.llm_provider;
        }
      } catch {
        // cortrix-agent may not be running
      }

      const status: SystemStatus = {
        version: health.version,
        uptime_seconds: health.uptime_seconds,
        namespace_count: nsList.length,
        total_doc_count: totalDocs,
        total_block_count: totalBlocks,
        queue_status: { pending: 0, processing: 0 },
        llm_provider: llmProvider || undefined,
        llm_enabled: llmEnabled,
      };
      set({ systemStatus: status });
    } catch {
      // silently fail
    }
  },

  globalError: null,
  setGlobalError: (error) => set({ globalError: error }),

  theme: (() => {
    if (typeof window === 'undefined') return 'light';
    const stored = localStorage.getItem('cortrix-theme');
    if (stored === 'dark' || stored === 'light') return stored;
    return window.matchMedia('(prefers-color-scheme: dark)').matches ? 'dark' : 'light';
  })(),
  toggleTheme: () =>
    set((s) => {
      const next = s.theme === 'dark' ? 'light' : 'dark';
      localStorage.setItem('cortrix-theme', next);
      if (next === 'dark') {
        document.documentElement.classList.add('dark');
      } else {
        document.documentElement.classList.remove('dark');
      }
      return { theme: next };
    }),
}));
