import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import { listNamespaces, createNamespace, deleteNamespace } from './namespaces';

// namespaces api tests (web UI — catalog NS CRUD client contract). Asserts each
// helper hits the right endpoint/verb and unwraps the response shape.

const mockFetch = vi.fn();

function ok(data: unknown, status = 200) {
  return Promise.resolve({
    ok: true,
    status,
    headers: new Headers({ 'content-type': 'application/json' }),
    json: () => Promise.resolve(data),
    text: () => Promise.resolve(JSON.stringify(data)),
  } as Response);
}

beforeEach(() => {
  vi.stubGlobal('fetch', mockFetch);
  mockFetch.mockReset();
});
afterEach(() => vi.unstubAllGlobals());

describe('namespaces api', () => {
  it('listNamespaces unwraps the namespaces array', async () => {
    mockFetch.mockReturnValueOnce(ok({ namespaces: [{ name: 'default' }, { name: 'legal' }] }));
    const out = await listNamespaces();
    expect(out.map((n) => n.name)).toEqual(['default', 'legal']);
    expect(mockFetch.mock.calls[0][0]).toContain('/api/v1/namespaces');
  });

  it('createNamespace POSTs the name', async () => {
    mockFetch.mockReturnValueOnce(ok({ name: 'docs', created: true }));
    await createNamespace('docs');
    const [url, init] = mockFetch.mock.calls[0];
    expect(url).toContain('/api/v1/namespaces');
    expect(init.method).toBe('POST');
    expect(init.body).toBe(JSON.stringify({ name: 'docs' }));
  });

  it('deleteNamespace DELETEs the named resource', async () => {
    mockFetch.mockReturnValueOnce(
      Promise.resolve({
        ok: true,
        status: 204,
        headers: new Headers({ 'content-length': '0' }),
        text: () => Promise.resolve(''),
        json: () => Promise.resolve(undefined),
      } as Response),
    );
    await deleteNamespace('docs');
    const [url, init] = mockFetch.mock.calls[0];
    expect(url).toContain('/api/v1/namespaces/docs');
    expect(init.method).toBe('DELETE');
  });
});
