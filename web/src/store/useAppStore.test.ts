import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import { useAppStore } from './useAppStore';
import { useFeatureFlagsStore } from '../hooks/useFeatureFlags';

// Mock fetch for API calls in jsdom environment.
//
// Isolation (R5/S11 flaky fix): each test gets a FRESH mock — mockReset() in
// beforeEach drops every queued mockReturnValueOnce + clears call history so a
// rejection queued by one test can never leak into the next (the previous
// shared-queue ordering was the root cause of the 2 "flaky" failures). The
// stores are also reset to their initial slice so a value set by an earlier
// test (e.g. namespaces) doesn't satisfy a later assertion.
const mockFetch = vi.fn();

function makeStorage(): Storage {
  const data = new Map<string, string>();
  return {
    get length() {
      return data.size;
    },
    clear: () => data.clear(),
    getItem: (key: string) => data.get(key) ?? null,
    key: (index: number) => Array.from(data.keys())[index] ?? null,
    removeItem: (key: string) => { data.delete(key); },
    setItem: (key: string, value: string) => { data.set(key, value); },
  };
}

beforeEach(() => {
  const storage = makeStorage();
  vi.stubGlobal('fetch', mockFetch);
  vi.stubGlobal('localStorage', storage);
  Object.defineProperty(window, 'localStorage', {
    value: storage,
    configurable: true,
  });
  mockFetch.mockReset();
  window.localStorage?.clear?.();
  useAppStore.setState({
    currentNamespace: 'default',
    namespaces: [],
    systemStatus: null,
    globalError: null,
  });
});

afterEach(() => {
  vi.unstubAllGlobals();
});

// Spec-complete Response stub. The shared api client (src/api/client.ts) reads
// res.headers.get('content-length') after a 2xx, so the mock MUST expose a real
// Headers object — omitting it (the old helper did) made client.ts throw a
// TypeError that the store swallowed, leaving state empty. That, not timing,
// is why "loads namespaces" / "loads system status" failed.
function jsonResponse(data: unknown) {
  const body = JSON.stringify(data);
  return Promise.resolve({
    ok: true,
    status: 200,
    headers: new Headers({ 'content-type': 'application/json' }),
    json: () => Promise.resolve(data),
    text: () => Promise.resolve(body),
  } as Response);
}

describe('useAppStore', () => {
  it('has default namespace', () => {
    const state = useAppStore.getState();
    expect(state.currentNamespace).toBe('default');
  });

  it('switches namespace', () => {
    useAppStore.getState().setCurrentNamespace('legal-docs');
    expect(useAppStore.getState().currentNamespace).toBe('legal-docs');
    // Reset
    useAppStore.getState().setCurrentNamespace('default');
  });

  // Persistence (R9 bug): the selection must survive a reload/remount, so
  // setCurrentNamespace mirrors it to localStorage (same hand-rolled pattern as
  // the theme toggle). initialNamespace() reads this key on the next boot.
  it('persists the current namespace to localStorage', () => {
    useAppStore.getState().setCurrentNamespace('finance');
    expect(localStorage.getItem('cortrix-namespace')).toBe('finance');
    // Reset so neither the store nor the persisted key leaks into later tests.
    useAppStore.getState().setCurrentNamespace('default');
    expect(localStorage.getItem('cortrix-namespace')).toBe('default');
  });

  // Page navigation moved to react-router (R3/S5); the store no longer tracks
  // `activePage`. The remaining store-owned UI concern is the theme toggle.
  it('toggles theme', () => {
    const initial = useAppStore.getState().theme;
    useAppStore.getState().toggleTheme();
    expect(useAppStore.getState().theme).toBe(initial === 'dark' ? 'light' : 'dark');
    // Reset to the initial value so other tests are unaffected.
    useAppStore.getState().toggleTheme();
    expect(useAppStore.getState().theme).toBe(initial);
  });

  it('loads namespaces', async () => {
    mockFetch.mockReturnValueOnce(jsonResponse({
      namespaces: [
        { name: 'default', doc_count: 5, block_count: 100 },
        { name: 'legal', doc_count: 2, block_count: 30 },
      ],
    }));

    await useAppStore.getState().loadNamespaces();
    const ns = useAppStore.getState().namespaces;
    expect(ns.length).toBeGreaterThan(0);
    expect(ns[0].name).toBe('default');
  });

  it('uses the first real namespace when the persisted namespace is missing', async () => {
    useAppStore.getState().setCurrentNamespace('default');
    mockFetch.mockReturnValueOnce(jsonResponse({
      namespaces: [
        { name: 'legal', doc_count: 2, block_count: 30 },
        { name: 'finance', doc_count: 1, block_count: 10 },
      ],
    }));

    await useAppStore.getState().loadNamespaces();

    expect(useAppStore.getState().currentNamespace).toBe('legal');
    expect(window.localStorage?.getItem?.('cortrix-namespace')).toBe('legal');
  });

  // Feature flags moved to their own store in R4/S6 (useFeatureFlagsStore,
  // P02a § 5.3). On a failed fetch it falls back to the CE default with no
  // enabled features; the Ent placeholder probes return false.
  it('feature flags fall back to CE on error', async () => {
    // mockImplementationOnce (not mockReturnValueOnce(Promise.reject)) so the
    // rejected promise is created lazily on call and consumed in-band — avoids
    // the "unhandled rejection" Vitest warning from an eagerly-built reject.
    mockFetch.mockImplementationOnce(() => Promise.reject(new Error('not found')));

    await useFeatureFlagsStore.getState().loadFeatures();
    const state = useFeatureFlagsStore.getState();
    expect(state.edition).toBe('ce');
    expect(state.isEnabled('text_to_sql')).toBe(false);
  });

  // Degrade branch: a failed namespace fetch must not throw — it records a
  // globalError and leaves the namespace list empty (no partial/corrupt state).
  it('records globalError and keeps namespaces empty when the fetch fails', async () => {
    mockFetch.mockImplementationOnce(() => Promise.reject(new Error('network down')));

    await expect(useAppStore.getState().loadNamespaces()).resolves.toBeUndefined();
    expect(useAppStore.getState().namespaces).toEqual([]);
    expect(useAppStore.getState().globalError).toBeTruthy();
  });

  // Null/empty branch: an empty namespace list keeps the current selection
  // (resolveCurrentNamespace falls back to `current` when there is no first ns).
  it('keeps the current namespace when the backend returns no namespaces', async () => {
    useAppStore.getState().setCurrentNamespace('default');
    mockFetch.mockReturnValueOnce(jsonResponse({ namespaces: [] }));

    await useAppStore.getState().loadNamespaces();

    expect(useAppStore.getState().namespaces).toEqual([]);
    expect(useAppStore.getState().currentNamespace).toBe('default');
  });

  // Degrade branch: a failed health probe must fail silently — systemStatus
  // stays null and the call resolves (the header just renders nothing rather
  // than crashing the app).
  it('leaves systemStatus null when the health probe fails', async () => {
    mockFetch.mockImplementationOnce(() => Promise.reject(new Error('health unreachable')));

    await expect(useAppStore.getState().loadSystemStatus()).resolves.toBeUndefined();
    expect(useAppStore.getState().systemStatus).toBeNull();
  });

  it('loads system status with LLM info', async () => {
    // First call: /api/v1/health
    mockFetch.mockReturnValueOnce(jsonResponse({
      version: '1.0.0-rc.2',
      status: 'healthy',
      uptime_seconds: 120,
      llm_enabled: true,
      llm_provider: 'openai',
      llm_model: 'gpt-4',
    }));
    // Second call: /agent/config (may fail, that's OK — lazy reject, see above).
    mockFetch.mockImplementationOnce(() => Promise.reject(new Error('agent not running')));

    // Pre-populate namespaces so the reducer can compute totals
    useAppStore.setState({
      namespaces: [
        { name: 'default', created_at: '2026-01-01T00:00:00Z', doc_count: 3, block_count: 50 },
      ],
    });

    await useAppStore.getState().loadSystemStatus();
    const status = useAppStore.getState().systemStatus;
    expect(status).toBeTruthy();
    expect(status!.version).toBe('1.0.0-rc.2');
    // F4: mock API returns LLM fields
    expect(status!.llm_enabled).toBeDefined();
    expect(status!.llm_provider).toBeDefined();
  });
});
