import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import { useSearchStore } from './useSearchStore';

const mockFetch = vi.fn();

beforeEach(() => {
  vi.stubGlobal('fetch', mockFetch);
  mockFetch.mockReset();
  // Fresh store slice per test (R5/S11 isolation).
  useSearchStore.setState({ query: '', results: [], meta: null, error: null });
});

afterEach(() => {
  vi.unstubAllGlobals();
});

// Spec-complete Response stub (incl. Headers) so the real api client path is
// exercised — see useAppStore.test.ts for why the headers object is required.
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

describe('useSearchStore', () => {
  it('starts with empty query', () => {
    expect(useSearchStore.getState().query).toBe('');
    expect(useSearchStore.getState().results).toEqual([]);
  });

  it('updates query', () => {
    useSearchStore.getState().setQuery('test query');
    expect(useSearchStore.getState().query).toBe('test query');
  });

  it('performs search and returns results', async () => {
    mockFetch.mockReturnValueOnce(jsonResponse({
      results: [
        {
          block_id: 1,
          doc_id: 1,
          content: 'Architecture overview',
          score: 0.95,
          block_type: 'FILE',
          metadata: {},
        },
      ],
      meta: {
        latency_ms: 42,
        total_results: 1,
      },
    }));

    useSearchStore.getState().setQuery('architecture');
    await useSearchStore.getState().search();
    const { results, meta } = useSearchStore.getState();
    expect(results.length).toBeGreaterThan(0);
    expect(meta).toBeTruthy();
    expect(meta!.latency_ms).toBeDefined();
  });
});
