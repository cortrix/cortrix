import { create } from 'zustand';
import type { ChatMessage } from '../types/state';
import type { SearchResult } from '../types/api';
import { useAppStore } from './useAppStore';
import { getCsrfToken, CSRF_HEADER } from '../utils/csrf';
import { mockApi } from '../api/mock';
import { USE_MOCK } from '../api/fallback';
import { errorMessage } from '../api/errors';

// F48 Cortrix Agent endpoint (P02a § 4.1, F48-rev-1): chat is served by
// cortrix-server's reverse proxy at /api/v1/agent/chat (same-origin SSE +
// multi-tenant header pass-through). The legacy MVP path was the agent-demo
// `/agent/chat`; R4/S2 moves to the unified endpoint.
const AGENT_CHAT_ENDPOINT = '/api/v1/agent/chat';

interface ChatSession {
  session_id: string;
  title: string;
  created_at: string;
  updated_at?: string;
  interaction_count?: number;
}

interface ChatState {
  messages: ChatMessage[];
  isStreaming: boolean;
  sessionId: string;
  sessions: ChatSession[];
  cancelStream: (() => void) | null;
  sendMessage: (text: string) => void;
  stopStreaming: () => void;
  clearMessages: () => void;
  loadSessions: () => Promise<void>;
  newSession: () => void;
  switchSession: (sid: string) => Promise<void>;
  deleteSession: (sid: string) => Promise<void>;
}

export const useChatStore = create<ChatState>((set, get) => ({
  messages: [],
  isStreaming: false,
  sessionId: crypto.randomUUID(),
  sessions: [],
  cancelStream: null,

  sendMessage: (text) => {
    if (!text.trim() || get().isStreaming) return;

    const userMsg: ChatMessage = {
      id: `msg-${Date.now()}-user`,
      role: 'user',
      content: text,
      timestamp: new Date().toISOString(),
    };

    const assistantId = `msg-${Date.now()}-ai`;
    const assistantMsg: ChatMessage = {
      id: assistantId,
      role: 'assistant',
      content: '',
      timestamp: new Date().toISOString(),
    };

    const abortController = new AbortController();

    set((s) => ({
      messages: [...s.messages, userMsg, assistantMsg],
      isStreaming: true,
      cancelStream: () => abortController.abort(),
    }));

    const sessionId = get().sessionId;
    const namespace = useAppStore.getState().currentNamespace;

    const applyContent = (content: string) =>
      set((s) => ({
        messages: s.messages.map((m) => (m.id === assistantId ? { ...m, content } : m)),
      }));

    const finalize = (content: string, sources: SearchResult[]) =>
      set((s) => ({
        messages: s.messages.map((m) =>
          m.id === assistantId
            ? { ...m, content, sources: sources.length > 0 ? sources : undefined }
            : m,
        ),
        isStreaming: false,
        cancelStream: null,
      }));

    // F48 multi-tenant header pass-through (§ 8.2): X-Cortrix-Namespace is the
    // request-level NS override. X-Cortrix-Tenant-Id is a Cloud (V1.5) concern
    // — CE is single-tenant so it is omitted here (cortrix-server ignores an
    // absent header). CSRF header is required on this POST (§ 4.6).
    const headers: Record<string, string> = {
      'Content-Type': 'application/json',
      [CSRF_HEADER]: getCsrfToken(),
      'X-Cortrix-Namespace': namespace,
    };

    fetch(AGENT_CHAT_ENDPOINT, {
      method: 'POST',
      credentials: 'include',
      headers,
      body: JSON.stringify({ message: text, session_id: sessionId }),
      signal: abortController.signal,
    })
      .then(async (response) => {
        if (!response.ok) throw new Error(`HTTP ${response.status}`);
        const reader = response.body?.getReader();
        if (!reader) throw new Error('No response body');

        const decoder = new TextDecoder();
        let buffer = '';
        let fullContent = '';
        let sources: SearchResult[] = [];

        while (true) {
          const { done, value } = await reader.read();
          if (done) break;

          buffer += decoder.decode(value, { stream: true });
          const lines = buffer.split('\n');
          buffer = lines.pop() || '';

          for (const line of lines) {
            if (!line.startsWith('data:')) continue;
            const dataStr = line.slice(5).trim();
            if (dataStr === '[DONE]') continue;
            try {
              const data = JSON.parse(dataStr);
              // F48 SSE: `{ "chunk": "…" }` tokens, then a final
              // `{ "meta": { chunks_used, chunk_ids, rag_status, … } }` frame.
              if (typeof data.chunk === 'string') {
                fullContent += data.chunk;
                applyContent(fullContent);
              }
              // Legacy/mock path may stream `sources`. F48 carries real per-source
              // provenance in `meta.citations` (source_path + score + snippet) — map
              // those so each source shows real text + score. Fall back to bare
              // `chunk_ids` only for an older agent that predates citations.
              if (Array.isArray(data.sources)) {
                sources = data.sources;
              } else if (data.meta && Array.isArray(data.meta.citations)) {
                sources = data.meta.citations.map(
                  (c: {
                    chunk_id?: string;
                    source_path?: string;
                    score?: number;
                    snippet?: string;
                  }) => ({
                    child_id: c.chunk_id ?? '',
                    parent_id: '',
                    content: c.snippet ?? '',
                    parent_content: '',
                    score: typeof c.score === 'number' ? c.score : 0,
                    rerank_score: typeof c.score === 'number' ? c.score : 0,
                    namespace,
                    content_hash: '',
                    metadata: c.source_path ? { source_path: c.source_path } : undefined,
                  }),
                );
              } else if (data.meta && Array.isArray(data.meta.chunk_ids)) {
                sources = data.meta.chunk_ids.map((cid: string) => ({
                  child_id: cid,
                  parent_id: '',
                  content: '',
                  parent_content: '',
                  score: 0,
                  rerank_score: 0,
                  namespace,
                  content_hash: '',
                }));
              }
            } catch {
              // ignore malformed SSE frames
            }
          }
        }

        finalize(fullContent, sources);
        // Refresh session list after a successful exchange.
        get().loadSessions();
      })
      .catch((err) => {
        if (err.name === 'AbortError') {
          set((s) => ({
            messages: s.messages.map((m) =>
              m.id === assistantId
                ? { ...m, content: (m.content || '') + '\n\n*[Stopped]*' }
                : m,
            ),
            isStreaming: false,
            cancelStream: null,
          }));
          return;
        }
        // Standalone (D3): backend unreachable (fetch throws TypeError) → stream
        // the mock SSE so the chat demo works without cortrix-server. A real
        // HTTP error surfaces a message instead. The mock is build-time gated
        // (./fallback.ts) so it never runs in a production build.
        if (USE_MOCK && err instanceof TypeError) {
          let mockContent = '';
          let mockSources: SearchResult[] = [];
          const cancel = mockApi.streamChat(
            namespace,
            text,
            (tok) => {
              mockContent += tok;
              applyContent(mockContent);
            },
            (srcs) => {
              mockSources = srcs;
            },
            () => {
              finalize(mockContent, mockSources);
              get().loadSessions();
            },
            (msg) => finalize(`Error: ${msg}`, []),
          );
          set({ cancelStream: cancel });
          return;
        }
        finalize(`Error: ${errorMessage(err)}`, []);
      });
  },

  stopStreaming: () => {
    const cancel = get().cancelStream;
    if (cancel) cancel();
  },

  clearMessages: () =>
    set({
      messages: [],
      sessionId: crypto.randomUUID(),
    }),

  loadSessions: async () => {
    try {
      const ns = useAppStore.getState().currentNamespace;
      const res = await fetch(`/api/v1/memory/sessions?namespace=${ns}&limit=50`);
      if (!res.ok) return;
      const data = await res.json();
      // Filter out sessions with no interactions
      const sessions = (data.sessions || []).filter(
        (s: ChatSession) => (s.interaction_count ?? 0) > 0,
      );
      set({ sessions });
    } catch {
      // ignore
    }
  },

  newSession: () => {
    set({
      messages: [],
      sessionId: crypto.randomUUID(),
    });
  },

  deleteSession: async (sid) => {
    try {
      const ns = useAppStore.getState().currentNamespace;
      const res = await fetch(`/api/v1/memory/sessions/${sid}?namespace=${ns}`, {
        method: 'DELETE',
      });
      if (!res.ok) return;
      // If we deleted the active session, start a new one
      if (get().sessionId === sid) {
        get().newSession();
      }
      // Refresh session list
      await get().loadSessions();
    } catch {
      // ignore
    }
  },

  switchSession: async (sid) => {
    set({ messages: [], sessionId: sid });
    // Load session history from backend (call cortrix-server directly, same as loadSessions)
    try {
      const ns = useAppStore.getState().currentNamespace;
      const res = await fetch(`/api/v1/memory/sessions/${sid}?namespace=${ns}`);
      if (!res.ok) return;
      const data = await res.json();
      const interactions = data.interactions || [];
      const msgs: ChatMessage[] = interactions.map((item: any, idx: number) => ({
        id: `hist-${idx}`,
        role: item.role as 'user' | 'assistant',
        content: item.metadata?.original_content || item.content,
        timestamp: item.created_at,
      }));
      set({ messages: msgs });
    } catch {
      // session may not have interactions yet
    }
  },
}));
